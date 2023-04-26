/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   https://lammps.sandia.gov/, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Yutack Park (SNU)
------------------------------------------------------------------------- */

#include <ATen/core/Dict.h>
#include <ATen/core/ivalue_inl.h>
#include <ATen/ops/from_blob.h>
#include <c10/core/Scalar.h>
#include <c10/core/TensorOptions.h>
#include <limits>
#include <numeric>
#include <string>

#include <torch/csrc/jit/api/module.h>
#include <torch/torch.h>
#include <torch/script.h>

#include <cuda_runtime.h>

#include "memory.h"
#include "error.h"
#include "atom.h"
#include "force.h"
#include "neighbor.h" 
#include "neigh_list.h"
#include "neigh_request.h"
#include "comm.h"
#include "comm_brick.h"
#include "error.h"
//#include "nvToolsExt.h"

#include <cassert>
#include "pair_e3gnn_parallel.h"
//#include "mpi-ext.h" //This should be included after mpi.h which is included in pair.h

using namespace LAMMPS_NS;

#define INTEGER_TYPE torch::TensorOptions().dtype(torch::kInt64)
#define FLOAT_TYPE torch::TensorOptions().dtype(torch::kFloat)

DeviceBuffManager& DeviceBuffManager::getInstance(){
  static DeviceBuffManager instance;
  return instance;
}

void DeviceBuffManager::get_buffer(int send_size,int recv_size, float*& buf_send_ptr, float*& buf_recv_ptr){
  if(send_size > send_buf_size) {
    cudaFree(buf_send_device);
    cudaError_t cuda_err = cudaMalloc(&buf_send_device, send_size*sizeof(float));
    send_buf_size = send_size;
  }
  if(recv_size > recv_buf_size) {
    cudaFree(buf_recv_device);
    cudaError_t cuda_err = cudaMalloc(&buf_recv_device, recv_size*sizeof(float));
    recv_buf_size = recv_size;
  }
  buf_send_ptr = buf_send_device;
  buf_recv_ptr = buf_recv_device;
}

DeviceBuffManager::~DeviceBuffManager() {
  cudaFree(buf_send_device);
  cudaFree(buf_recv_device);
}

PairE3GNNParallel::PairE3GNNParallel(LAMMPS *lmp) : Pair(lmp) {
  // constructor
  std::string device_name;
  if(torch::cuda::is_available()){
    device = get_cuda_device();
    device_name = "CUDA";
  } else {
    device = torch::kCPU;
    device_name = "CPU";
  }

  comm_forward = 0;
  comm_reverse = 0;

#if defined(MPIX_CUDA_AWARE_SUPPORT)
    if (1 == MPIX_Query_cuda_support()) {
      use_cuda_mpi=true;
    } else {
      use_cuda_mpi=false;
    }
#else 
    use_cuda_mpi=false;
#endif
  //TODO: GPU aware MPI detection (for now, only OpenMPI works)

  if (lmp->logfile) {
    fprintf(lmp->logfile, "PairE3GNNParallel using device : %s\n", device_name.c_str());
    fprintf(lmp->logfile, "PairE3GNNParallel cuda-award mpi: %s\n", use_cuda_mpi? "True" : "False");
  }
  //I will not support GPU device + cuda-'NOT'aware mpi combination.
}

torch::Device PairE3GNNParallel::get_cuda_device() {
  char* cuda_visible = std::getenv("CUDA_VISIBLE_DEVICES");
  int num_gpus;
  int idx;
  int rank = comm->me;
  if(cuda_visible == nullptr){
    // assume every gpu in node is avail
    num_gpus = torch::cuda::device_count();
    //believe user did right thing...
    idx = rank % num_gpus;
  } else {
    auto delim = ",";
    char *tok = std::strtok(cuda_visible, delim);
    std::vector<std::string> device_ids;
    while(tok != nullptr) {
      device_ids.push_back(std::string(tok));
      tok = std::strtok(nullptr, delim);
    }
    idx = std::stoi(device_ids[rank % device_ids.size()]);
  }
  cudaError_t cuda_err = cudaSetDevice(idx);
  /*if (cuda_err != cudaSuccess) {
    std::cerr << "Failed to set CUDA device: " << cudaGetErrorString(cuda_err) << std::endl;
  }*/
  return torch::Device(torch::kCUDA, idx);
}

PairE3GNNParallel::~PairE3GNNParallel() {
  if(allocated) {
    memory->destroy(setflag);
    memory->destroy(cutsq);
    memory->destroy(map);
  }
}

int PairE3GNNParallel::get_x_dim() {
  return x_dim;
}

bool PairE3GNNParallel::use_cuda_mpi_() {
  return use_cuda_mpi;
} 

bool PairE3GNNParallel::is_comm_preprocess_done() {
  return comm_preprocess_done;
}

void PairE3GNNParallel::compute(int eflag, int vflag) {
  /*
     Graph build on cpu
  */
  //nvtxRangePushA("compute");
  if (eflag || vflag) ev_setup(eflag,vflag);
  else evflag = vflag_fdotr = 0;
  if(vflag_atom) {
    error->all(FLERR,"atomic stress related feature is not supported\n");
  }

  double **x = atom->x;
  double **f = atom->f;
  int *type = atom->type;
  int nlocal = list->inum;  // same as nlocal
  int nghost = atom->nghost;
  int ntotal = nlocal + nghost;
  int* ilist = list->ilist;
  int inum = list->inum;

  CommBrick* comm_brick = dynamic_cast<CommBrick*>(comm);
  if(comm_brick == nullptr) {
    error->all(FLERR,"e3gnn/parallel: comm style should be brick & modified codes");
  }

  bigint natoms = atom->natoms;
  tagint *tag = atom->tag;
  int tag_to_graph_idx[natoms+1];  // tag starts from 1 not 0
  std::fill_n(tag_to_graph_idx, natoms+1, -1);
  tag_to_graph_idx_ptr = tag_to_graph_idx;

  int graph_indexer = nlocal;
  int graph_index_to_i[ntotal];

  int* numneigh = list->numneigh;  // j loop cond
  int** firstneigh = list->firstneigh;  // j list
  const int nedges_upper_bound = std::accumulate(numneigh, numneigh+nlocal, 0);

  std::vector<long> node_type;
  std::vector<long> node_type_ghost;

  float edge_vec[nedges_upper_bound][3];
  long edge_idx_src[nedges_upper_bound];
  long edge_idx_dst[nedges_upper_bound];

  int nedges = 0;
  for (int ii = 0; ii < inum; ii++) {
    // populate tag_to_graph_idx of local atoms
    const int i = ilist[ii];
    const int itag = tag[i];
    const int itype = type[i];
    tag_to_graph_idx[itag] = ii;
    graph_index_to_i[ii] = i;
    node_type.push_back(map[itype]);
  }

  //loop over neighbors, build graph
  for (int ii = 0; ii < inum; ii++) {
    const int i = ilist[ii];
    const int i_graph_idx = ii;
    const int* jlist = firstneigh[i];
    const int jnum = numneigh[i];

    for (int jj = 0; jj < jnum; jj++) {
      int j = jlist[jj];
      const int jtag = tag[j];
      j &= NEIGHMASK;
      const int jtype = type[j];
      // we have to calculate Rij to check cutoff in lammps side
      const double delij[3] = {x[j][0] - x[i][0], x[j][1] - x[i][1], x[j][2] - x[i][2]};
      const double Rij = delij[0]*delij[0] + delij[1]*delij[1] + delij[2]*delij[2];

      int j_graph_idx;
      if(Rij < cutoff_square) {
        // if given j is not local atom
        if(tag_to_graph_idx[jtag] == -1) {
          tag_to_graph_idx[jtag] = graph_indexer;
          graph_index_to_i[graph_indexer] = j;
          node_type_ghost.push_back(map[jtype]);
          graph_indexer++;
        }

        j_graph_idx = tag_to_graph_idx[jtag];
        edge_idx_src[nedges] = i_graph_idx;
        edge_idx_dst[nedges] = j_graph_idx;
        edge_vec[nedges][0] = delij[0];
        edge_vec[nedges][1] = delij[1];
        edge_vec[nedges][2] = delij[2];
        nedges++;
      }
    } // j loop end
  } // i loop end

  //memeber variable
  graph_size = graph_indexer;
  const int ghost_node_num = graph_size - nlocal;

  // convert data to Tensor
  auto inp_node_type = torch::from_blob(node_type.data(), nlocal, INTEGER_TYPE);
  auto inp_node_type_ghost = torch::from_blob(node_type_ghost.data(), ghost_node_num, INTEGER_TYPE);

  long num_nodes[1] = {long(nlocal)};
  auto inp_num_atoms = torch::from_blob(num_nodes, {1}, INTEGER_TYPE);

  auto edge_idx_src_tensor = torch::from_blob(edge_idx_src, {nedges}, INTEGER_TYPE);
  auto edge_idx_dst_tensor = torch::from_blob(edge_idx_dst, {nedges}, INTEGER_TYPE);
  auto inp_edge_index = torch::stack({edge_idx_src_tensor, edge_idx_dst_tensor});

  auto inp_edge_vec = torch::from_blob(edge_vec, {nedges, 3}, FLOAT_TYPE);

  // r_original requires grad True
  inp_edge_vec.set_requires_grad(true);

  torch::Dict<std::string, torch::Tensor> input_dict;
  input_dict.insert("x", inp_node_type.to(device));
  input_dict.insert("x_ghost", inp_node_type_ghost.to(device));
  input_dict.insert("edge_index", inp_edge_index.to(device));
  input_dict.insert("edge_vec", inp_edge_vec.to(device));
  input_dict.insert("num_atoms", inp_num_atoms.to(device));
  input_dict.insert("nlocal", inp_num_atoms.to(torch::kCPU));

  std::list<std::vector<torch::Tensor>> wrt_tensors;
  wrt_tensors.push_back({input_dict.at("edge_vec")});

  auto model_part = model_list.front();
  //nvtxRangePushA("forward_whole");

  //nvtxRangePushA("forward_0");
  auto output = model_part.forward({input_dict}).toGenericDict();
  //nvtxRangePop();

  //nvtxRangePushA("comm_preprocess_forward");
  comm_preprocess();
  //nvtxRangePop();

  // extra_graph_idx_map is set from comm_preprocess();
  const int extra_size = ghost_node_num + static_cast<int>(extra_graph_idx_map.size());
  torch::Tensor x_local;
  torch::Tensor x_ghost;

  for(auto it = model_list.begin(); it != model_list.end(); ++it) {
    if(it == model_list.begin()) continue;
    //nvtxRangePushA("comm_forward_x_&_process");
    model_part = *it;

    x_local = output.at("x").toTensor().detach().to(device);
    x_dim = x_local.size(1); // length of per atom vector(node feature)

    auto ghost_and_extra_x = torch::zeros({ghost_node_num + extra_size, x_dim}, FLOAT_TYPE.device(device));
    x_comm = torch::cat({x_local, ghost_and_extra_x}, 0).to(device);

    //nvtxRangePushA("comm_forward");
    comm_brick->forward_comm(this); //populate x_ghost by communication
    //nvtxRangePop();

    //What we got from forward_comm
    x_ghost = torch::split_with_sizes(x_comm, {nlocal, ghost_node_num, extra_size}, 0)[1];
    x_ghost.set_requires_grad(true);

    //prepare next input (output > next input)
    output.insert_or_assign("x_ghost", x_ghost.to(device));
    //make another edge_vec to discriminate grad calculation with other edge_vecs(maybe redundant?)
    output.insert_or_assign("edge_vec", output.at("edge_vec").toTensor().clone());

    //save tensors for backprop
    wrt_tensors.push_back({output.at("edge_vec").toTensor(), \
                           output.at("x").toTensor(), \
                           output.at("self_cont_tmp").toTensor(), \
                           output.at("x_ghost").toTensor()});
    //nvtxRangePop();

    //nvtxRangePushA("forward_x");
    output = model_part.forward({output}).toGenericDict();
    //nvtxRangePop();
  }
  torch::Tensor scaled_energy_tensor = output.at("scaled_total_energy").toTensor().squeeze();
  //nvtxRangePop();

  //nvtxRangePushA("backprop_whole");
  torch::Tensor dE_dr = torch::zeros({nedges, 3}, FLOAT_TYPE.device(device)); //create on device
  torch::Tensor x_local_save; //holds grad info of x_local (it loses its grad when sends to CPU)
  torch::Tensor self_conn_grads;
  std::vector<torch::Tensor> grads;
  std::vector<torch::Tensor> of_tensor;

  //TODO: most values of self_conn_grads were zero becuase we use only scalars for energy
  for(auto rit = wrt_tensors.rbegin(); rit != wrt_tensors.rend(); ++rit) {
    // edge_vec, x, x_ghost order
    auto wrt_tensor = *rit;
    if(rit == wrt_tensors.rbegin()) {
      //nvtxRangePushA("backprop_first");
      grads = torch::autograd::grad({scaled_energy_tensor}, wrt_tensor);
      //nvtxRangePop();
    } else {
      //nvtxRangePushA("backprop_mid");
      x_local_save.copy_(x_local);
      //                            of         wrt         grads_output
      grads = torch::autograd::grad(of_tensor, wrt_tensor, {x_local_save, self_conn_grads});
      //nvtxRangePop();
    }

    dE_dr = dE_dr + grads.at(0); //accumulate force
    if(std::distance(rit, wrt_tensors.rend()) == 1) continue;  // if last iteration
    //nvtxRangePushA("reverse_comm_&_process");

    of_tensor.clear();
    of_tensor.push_back(wrt_tensor[1]); // x
    of_tensor.push_back(wrt_tensor[2]); // self_cont_tmp

    x_local_save = grads.at(1);  // for grads_output
    x_local = x_local_save.detach();  // grad_outputs & communication
    x_dim = x_local.size(1);

    self_conn_grads = grads.at(2); // no communication, for grads_output

    x_ghost = grads.at(3).detach();  // yes communication, not for grads_output

    auto extra_x = torch::zeros({extra_size, x_dim}, FLOAT_TYPE.device(device));
    x_comm = torch::cat({x_local, x_ghost, extra_x}, 0).to(device);

    //nvtxRangePushA("reverse_comm");
    comm_brick->reverse_comm(this);  // completes x_local
    //nvtxRangePop();

    x_local = torch::split_with_sizes(x_comm, {nlocal, ghost_node_num, extra_size}, 0)[0];
    // now x_local is complete (dE_dx), become next grads_output(with self_conn_grads)

    //nvtxRangePop();
  }
  //nvtxRangePop();

  //postprocessing

  // TODO: atomic energy things?
  eng_vdwl += scaled_energy_tensor.item<float>()*scale + shift*nlocal; // accumulate energy

  dE_dr = dE_dr.to(torch::kCPU);
  torch::Tensor force_tensor = torch::zeros({graph_indexer, 3});

  // TODO:where I can find torch_scatter cpp version? I heard this version(torch defaults) is slower.
  force_tensor.scatter_(0, edge_idx_src_tensor.repeat_interleave(3).view({nedges,3}), dE_dr, "add");
  force_tensor.scatter_(0, edge_idx_dst_tensor.repeat_interleave(3).view({nedges,3}), torch::neg(dE_dr), "add");

  force_tensor = force_tensor.mul(scale);
  auto forces = force_tensor.accessor<float, 2>();

  for (int graph_idx=0; graph_idx < graph_indexer; graph_idx++){
    int i = graph_index_to_i[graph_idx];
    f[i][0] = forces[graph_idx][0];
    f[i][1] = forces[graph_idx][1];
    f[i][2] = forces[graph_idx][2];
  }

  // clean up comm preprocess variables
  comm_preprocess_done = false;
  for(int i=0; i<6; i++) {
    //array of vector<long>
    comm_index_pack_forward[i].clear();
    comm_index_unpack_forward[i].clear();
  }
  /*
  if(use_cuda_mpi) {
    //vector of tensor
    comm_index_pack_forward_tensor.clear();
    comm_index_unpack_forward_tensor.clear();
  }
  */
  extra_graph_idx_map.clear();
  //nvtxRangePop();
}

// allocate arrays (called from coeff)
void PairE3GNNParallel::allocate() {
  allocated = 1;
  int n = atom->ntypes;
  
  memory->create(setflag,n+1,n+1,"pair:setflag");
  memory->create(cutsq,n+1,n+1,"pair:cutsq");
  memory->create(map,n+1,"pair:map");
}

// global settings for pair_style
void PairE3GNNParallel::settings(int narg, char **arg) {
  if (narg != 0) {
    error->all(FLERR,"Illegal pair_style command");
  }
}

void PairE3GNNParallel::coeff(int narg, char **arg) {
  if(allocated) {
    error->all(FLERR,"pair_e3gnn coeff called twice");
  }
  allocate();

  if (strcmp(arg[0], "*") != 0 || strcmp(arg[1], "*") != 0) {
      error->all(FLERR, "e3gnn: firt and second input of pair_coeff should be '*'");
  }
  // expected input : pair_coeff * * pot.pth type_name1 type_name2 ...

  std::unordered_map<std::string, std::string> meta_dict = {
    {"chemical_symbols_to_index", ""},
    {"cutoff", ""},
    {"num_species", ""},
    {"model_type", ""},
    {"version", ""},
    {"dtype", ""},
    {"time", ""},
    {"shift", ""},
    {"scale", ""},
    {"comm_size", ""}
  };

  // model loading from input
  int n_model = std::stoi(arg[2]);
  for (int i=3; i<n_model+3; i++){
    model_list.push_back(torch::jit::load(std::string(arg[i]), device, meta_dict));
  }

  torch::jit::FusionStrategy strategy;
  // TODO: why first pew iteration is slower than nequip?
  strategy = {{torch::jit::FusionBehavior::DYNAMIC, 3}}; 
  torch::jit::setFusionStrategy(strategy);

  cutoff = std::stod(meta_dict["cutoff"]);
  shift = std::stod(meta_dict["shift"]);
  scale = std::stod(meta_dict["scale"]);

  // maximum possible size of per atom x before last convolution
  int comm_size = std::stod(meta_dict["comm_size"]);
  // to initialize buffer size for communication

  comm_forward = comm_size;
  comm_reverse = comm_size;

  cutoff_square = cutoff * cutoff;

  if(meta_dict["model_type"].compare("E3_equivariant_model") != 0){
    error->all(FLERR, "given model type is not E3_equivariant_model");    
  }

  std::string chem_str = meta_dict["chemical_symbols_to_index"];
  int ntypes = atom->ntypes;

  auto delim = " ";
  char *tok = std::strtok(const_cast<char*>(chem_str.c_str()), delim);
  std::vector<std::string> chem_vec;
  while (tok != nullptr){
    chem_vec.push_back(std::string(tok));
    tok = std::strtok(nullptr, delim);
  }

  // what if unkown chemical specie is in arg? should I abort? is there any use case for that?
  for (int i=3+n_model; i<narg; i++) {
    for (int j=0; j<chem_vec.size(); j++) {
      if (chem_vec[j].compare(arg[i]) == 0) {
        map[i-2-n_model] = j; //store from 1, (not 0)
      }
    }
  }

  for (int i = 1; i <= ntypes; i++) {
    for (int j = 1; j <= ntypes; j++) {
      if ((map[i] >= 0) && (map[j] >= 0)) {
          setflag[i][j] = 1;
          cutsq[i][j] = cutoff*cutoff;
      }
    }
  }

  if (lmp->logfile) {
    fprintf(lmp->logfile, "from simple_gnn version '%s' ", meta_dict["version"].c_str());
    fprintf(lmp->logfile, "%s precision model trained at %s is loaded\n", meta_dict["dtype"].c_str(), meta_dict["time"].c_str());
  }
}

// init specific to this pair
void PairE3GNNParallel::init_style() {
  //full neighbor list & newton on
  if (force->newton_pair == 0) {
    error->all(FLERR, "Pair style e3gnn/parallel requires newton pair on");    
  }
  neighbor->add_request(this, NeighConst::REQ_FULL);
}

double PairE3GNNParallel::init_one(int i, int j) {
  return cutoff;
}

void PairE3GNNParallel::comm_preprocess() {
  assert(!comm_preprocess_done);
  CommBrick* comm_brick = dynamic_cast<CommBrick*>(comm);
  comm_brick->forward_comm(this);
  for(int i=0; i<6; i++) {
    if(comm_index_pack_forward[i].size() == 0) {
      continue;
    }
    if(use_cuda_mpi){
      auto pmap = comm_index_pack_forward[i];
      auto upmap = comm_index_unpack_forward[i];
      comm_index_pack_forward_tensor[i] = torch::from_blob(pmap.data(), pmap.size(), INTEGER_TYPE).to(device);
      comm_index_unpack_forward_tensor[i] = torch::from_blob(upmap.data(), upmap.size(), INTEGER_TYPE).to(device);
    }
  }
  comm_preprocess_done = true;
}

void PairE3GNNParallel::pack_forward_init(int n, int *list_send, int comm_phase) {
  std::vector<long>& idx_map = comm_index_pack_forward[comm_phase];

  //nvtxRangePush("pack_forward_init");
  idx_map.reserve(n);

  int i,j;
  int nlocal = list->inum;
  tagint *tag = atom->tag;
  for (i = 0; i < n; i++) {
    int list_i = list_send[i];
    int graph_idx = tag_to_graph_idx_ptr[tag[list_i]];
    if(graph_idx != -1) {
      //known atom
      idx_map.push_back(graph_idx);
    } else {
      //unknown atom
      if(extra_graph_idx_map.find(list_i) != extra_graph_idx_map.end()){
        idx_map.push_back(extra_graph_idx_map[list_i]);
      } else {
        // unknown atom at pack forward, ghost atom outside cutoff?
        extra_graph_idx_map[i] = graph_size + extra_graph_idx_map.size();
        idx_map.push_back(extra_graph_idx_map[i]); //same as list_i in pack
      }
    }
  }
  //nvtxRangePop();
}

void PairE3GNNParallel::unpack_forward_init(int n, int first, int comm_phase) {
  std::vector<long>& idx_map = comm_index_unpack_forward[comm_phase];

  //nvtxRangePush("unpack_forward_init");
  idx_map.reserve(n);

  int i,j,last;
  last = first + n;
  int nlocal = list->inum;
  tagint *tag = atom->tag;

  for (i = first; i < last; i++) {
    int graph_idx = tag_to_graph_idx_ptr[tag[i]];
    if(graph_idx != -1) {
      idx_map.push_back(graph_idx);
    } else {
      extra_graph_idx_map[i] = graph_size + extra_graph_idx_map.size();
      idx_map.push_back(extra_graph_idx_map[i]); //same as list_i in pack
    }
  }
  //nvtxRangePop();
}

int PairE3GNNParallel::pack_forward_comm_gnn(float* buf, int comm_phase) {
  std::vector<long>& idx_map = comm_index_pack_forward[comm_phase];
  const int n = static_cast<int>(idx_map.size());

  //nvtxRangePush("pack_forward");
  if(use_cuda_mpi) {
    torch::Tensor& idx_map_tensor = comm_index_pack_forward_tensor[comm_phase];
    auto selected = x_comm.index_select(0, idx_map_tensor); //its size is x_dim * n
    cudaError_t cuda_err = cudaMemcpy(buf, selected.data_ptr<float>(), (x_dim*n)*sizeof(float), cudaMemcpyDeviceToDevice);

    // TODO: I want to remove temporary selected tensor for speed.
    // Code below produce wrong results. But if I change {n, x_dim} to {x_dim, n}, get correct result. 
    // Instead it produce warnings that the dimension is not correct so implicitly changed resulting tensor
    // to fit out_tensor(buf_tensor)'s dimension. How can I sovle this?

    //auto buf_tensor = torch::from_blob(buf, {n, x_dim}, FLOAT_TYPE.device(device)); // tensor wrapping of buf
    //at::index_select_out(buf_tensor, x_comm, 0, idx_map_tensor);
  } else {
    int i,j,m;
    m = 0;
    for (i = 0; i < n; i++) {
      const int idx = static_cast<int>(idx_map.at(i));
      float* from = x_comm[idx].data_ptr<float>();
      for (j = 0; j < x_dim; j++) {
        buf[m++] = from[j];
      }
    }
  }
  return x_dim*n;
}

void PairE3GNNParallel::unpack_forward_comm_gnn(float* buf, int comm_phase) {
  std::vector<long>& idx_map = comm_index_unpack_forward[comm_phase];
  const int n = static_cast<int>(idx_map.size());

  //nvtxRangePush("unpack_forward");
  if(use_cuda_mpi) {
    torch::Tensor& idx_map_tensor = comm_index_unpack_forward_tensor[comm_phase];
    // share same memory space with exisitng device buffer just wrapping to troch::Tensor
    auto buf_tensor = torch::from_blob(buf, {n, x_dim}, FLOAT_TYPE.device(device));
    x_comm.scatter_(0, idx_map_tensor.repeat_interleave(x_dim).view({n, x_dim}), buf_tensor);
  } else {
    int i,j,m;
    m = 0;
    for (i = 0; i < n; i++) {
      const int idx = static_cast<int>(idx_map.at(i));
      float* to = x_comm[idx].data_ptr<float>();
      for (j = 0; j < x_dim; j++) {
        to[j] = buf[m++];
      }
    }
  }
  //nvtxRangePop();
}

int PairE3GNNParallel::pack_reverse_comm_gnn(float* buf, int comm_phase) {
  std::vector<long>& idx_map = comm_index_unpack_forward[comm_phase];
  const int n = static_cast<int>(idx_map.size());

  //nvtxRangePush("pack_reverse");
  if(use_cuda_mpi) {
    torch::Tensor& idx_map_tensor = comm_index_unpack_forward_tensor[comm_phase];
    auto selected = x_comm.index_select(0, idx_map_tensor);
    cudaError_t cuda_err = cudaMemcpy(buf, selected.data_ptr<float>(), (x_dim*n)*sizeof(float), cudaMemcpyDeviceToDevice);
  } else {
    int i,j,m;
    m = 0;
    for (i = 0; i < n; i++) {
      const int idx = static_cast<int>(idx_map.at(i));
      float* from = x_comm[idx].data_ptr<float>();
      for (j = 0; j < x_dim; j++) {
        buf[m++] = from[j];
      }
    }
  }
  //nvtxRangePop();
  return x_dim*n;
}

void PairE3GNNParallel::unpack_reverse_comm_gnn(float *buf, int comm_phase) {
  std::vector<long>& idx_map = comm_index_pack_forward[comm_phase];
  const int n = static_cast<int>(idx_map.size());

  //nvtxRangePush("unpack_reverse");
  if(use_cuda_mpi) {
    torch::Tensor& idx_map_tensor = comm_index_pack_forward_tensor[comm_phase];
    auto buf_tensor = torch::from_blob(buf, {n, x_dim}, FLOAT_TYPE.device(device));
    x_comm.scatter_(0, idx_map_tensor.repeat_interleave(x_dim).view({n, x_dim}), buf_tensor, "add");
  } else {
    int i,j,m;
    m = 0;
    for (i = 0; i < n; i++) {
      const int idx = static_cast<int>(idx_map.at(i));
      float* to = x_comm[idx].data_ptr<float>();
      for (j = 0; j < x_dim; j++) {
        to[j] += buf[m++];
      }
    }
  }
  //nvtxRangePop();
}
