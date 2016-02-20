/*
  Author: Alex Long
  Date: 12/1/2015
  Name: transport_mesh_pass.h
*/

#ifndef transport_particle_pass_h_
#define transport_particle_pass_h_

#include <algorithm>
#include <boost/mpi.hpp>
#include <iostream>
#include <numeric>
#include <queue>
#include <map>
#include <stack>
#include <vector>

#include "constants.h"
#include "buffer.h"
#include "mesh.h"
#include "sampling_functions.h"
#include "RNG.h"
#include "photon.h"

namespace mpi = boost::mpi;

Constants::event_type transport_photon_particle_pass(Photon& phtn,
                                                      Mesh* mesh,
                                                      RNG* rng,
                                                      double& next_dt,
                                                      double& exit_E,
                                                      double& census_E,
                                                      std::vector<double>& rank_abs_E)
{
  using Constants::VACUUM; using Constants::REFLECT; 
  using Constants::ELEMENT; using Constants::PROCESSOR;
  using Constants::PASS; using Constants::CENSUS;
  using Constants::KILL; using Constants::EXIT;
  using Constants::bc_type;
  using Constants::event_type;
  using Constants::c;
  using std::min;

  uint32_t cell_id, next_cell;
  bc_type boundary_event;
  event_type event;
  double dist_to_scatter, dist_to_boundary, dist_to_census, dist_to_event;
  double sigma_a, sigma_s, f, absorbed_E;
  double angle[3];
  Cell cell;

  uint32_t surface_cross = 0;
  double cutoff_fraction = 0.01; //note: get this from IMC_state

  cell_id=phtn.get_cell();
  cell = mesh->get_on_rank_cell(cell_id);
  bool active = true;

  //transport this photon
  while(active) {
    sigma_a = cell.get_op_a();
    sigma_s = cell.get_op_s();
    f = cell.get_f();

    //get distance to event
    dist_to_scatter = -log(rng->generate_random_number())/((1.0-f)*sigma_a + sigma_s);
    dist_to_boundary = cell.get_distance_to_boundary(phtn.get_position(),
                                                      phtn.get_angle(),
                                                      surface_cross);
    dist_to_census = phtn.get_distance_remaining();

    //select minimum distance event
    dist_to_event = min(dist_to_scatter, min(dist_to_boundary, dist_to_census));

    //Calculate energy absorbed by material, update photon and material energy
    absorbed_E = phtn.get_E()*(1.0 - exp(-sigma_a*f*dist_to_event));
    phtn.set_E(phtn.get_E() - absorbed_E);

    rank_abs_E[cell_id] += absorbed_E;
    
    //update position
    phtn.move(dist_to_event);

    //Apply variance/runtime reduction
    if (phtn.below_cutoff(cutoff_fraction)) {
      rank_abs_E[cell_id] += phtn.get_E();
      phtn.set_dead();
      active=false;
      event=KILL;
    }
    // or apply event
    else {
      //Apply event
      //EVENT TYPE: SCATTER
      if(dist_to_event == dist_to_scatter) {
        get_uniform_angle(angle, rng);
        phtn.set_angle(angle);
      }
      //EVENT TYPE: BOUNDARY CROSS
      else if(dist_to_event == dist_to_boundary) {
        boundary_event = cell.get_bc(surface_cross);
        if(boundary_event == ELEMENT ) {
          next_cell = cell.get_next_cell(surface_cross);
          phtn.set_cell(next_cell);
          cell_id=next_cell;
          cell = mesh->get_on_rank_cell(cell_id);
        }
        else if(boundary_event == PROCESSOR) {
          active=false;
          //set correct cell index with global cell ID
          next_cell = cell.get_next_cell(surface_cross);
          phtn.set_cell(next_cell);
          event=PASS;
        }
        else if(boundary_event == VACUUM) {
          exit_E+=phtn.get_E();
          active=false; 
          event = EXIT;
        }
        else phtn.reflect(surface_cross); 
      }
      //EVENT TYPE: REACH CENSUS
      else if(dist_to_event == dist_to_census) {
        phtn.set_census_flag(true);
        phtn.set_distance_to_census(c*next_dt);
        active=false;
        event=CENSUS;
        census_E+=phtn.get_E();
      }
    } //end event loop
  } // end while alive
  return event;
}



std::vector<Photon> transport_particle_pass(Source& source,
                                            Mesh* mesh,
                                            IMC_State* imc_state,
                                            IMC_Parameters* imc_parameters,
                                            std::vector<double>& rank_abs_E,
                                            mpi::communicator world)
{
  using Constants::event_type;
  using Constants::PASS; using Constants::CENSUS;
  using Constants::KILL; using Constants::EXIT;
  using Constants::WAIT;
  using Constants::photon_tag;
  using std::queue;
  using std::vector;
  using std::stack;
  using std::map;
  using Constants::proc_null;
  using Constants::count_tag;
  using std::cout;
  using std::endl;

  double census_E=0.0;
  double exit_E = 0.0;
  double next_dt = imc_state->get_next_dt(); //!< Set for census photons
  double dt = imc_state->get_next_dt(); //<! For making current photons

  RNG *rng = imc_state->get_rng();

  int n_rank = world.size();
  int rank   = world.rank();

  // parallel event counters
  uint32_t n_photon_messages=0; //! Number of photon messages
  uint32_t n_photons_sent=0; //! Number of photons passed
  uint32_t n_sends_posted=0; //! Number of sent messages posted
  uint32_t n_sends_completed=0; //! Number of sent messages completed
  uint32_t n_receives_posted=0; //! Number of received messages completed
  uint32_t n_receives_completed=0; //! Number of received messages completed

  //get global photon count
  uint64_t n_local = source.get_n_photon();
  uint64_t n_global;
  MPI::COMM_WORLD.Allreduce(&n_local, 
                            &n_global, 
                           1, 
                           MPI_UNSIGNED_LONG, 
                           MPI_SUM);

  int parent = (rank + 1) / 2 - 1;
  int child1 = rank * 2 + 1;
  int child2 = child1 + 1;

  // set missing nodes to proc_null
  { 
    if (!rank)
        parent = proc_null;

    // maximum valid node id
    const int last_node = n_rank - 1;

    if (child1 > last_node)
    {
        child1 = proc_null;
        child2 = proc_null;
    }
    else if (child1 == last_node)
        child2 = proc_null;
  }

  // This flag indicates that send processing is needed for target rank
  vector<vector<Photon> > send_list;

  //Message requests for finished photon counts
  mpi::request c1_recv_request;
  mpi::request c2_recv_request;
  mpi::request p_recv_request;
  mpi::request c1_send_request;
  mpi::request c2_send_request; 
  mpi::request p_send_request;

  //Buffers for photon completed counts
  Buffer<uint64_t> c1_recv_buffer;
  Buffer<uint64_t> c2_recv_buffer;
  Buffer<uint64_t> p_recv_buffer;
  Buffer<uint64_t> c1_send_buffer;
  Buffer<uint64_t> c2_send_buffer;
  Buffer<uint64_t> p_send_buffer;

  //Get adjacent processor map (off_rank_id -> adjacent_proc_number)
  map<uint32_t, uint32_t> adjacent_procs = mesh->get_proc_adjacency_list();
  uint32_t n_adjacent = adjacent_procs.size();
  //Messsage requests for photon sends and receives
  mpi::request *phtn_recv_request   = new mpi::request[n_adjacent];
  mpi::request *phtn_send_request   = new mpi::request[n_adjacent];
  // make a send/receive particle buffer for each adjacent processor
  vector<Buffer<Photon> > phtn_recv_buffer(n_adjacent);
  vector<Buffer<Photon> > phtn_send_buffer(n_adjacent);

  // Messages are sent up the tree whenever a rank has completed its local work
  // or received an updated particle complete count from its child
  // Messages are sent down the tree only after completion and starting at the 
  // root node. 
  // Post receives for photon counts from children and parent now
  if (child1!=proc_null) {
    c1_recv_request = world.irecv(child1, count_tag, c1_recv_buffer.get_buffer());
    n_receives_posted++;
    c1_recv_buffer.set_awaiting();
  }
  if (child2!=proc_null) {
    c2_recv_request = world.irecv(child2, count_tag, c2_recv_buffer.get_buffer());
    n_receives_posted++;
    c2_recv_buffer.set_awaiting();
  }
  if (parent != proc_null) {
    p_recv_request = world.irecv(parent, count_tag, p_recv_buffer.get_buffer() );
    n_receives_posted++;
    p_recv_buffer.set_awaiting();
  }

  //Post receives for photons from adjacent sub-domains
  {
    uint32_t i_b; // buffer index
    int adj_rank; // adjacent rank
    for ( std::map<uint32_t, uint32_t>::iterator it=adjacent_procs.begin(); 
      it != adjacent_procs.end(); ++it) {
      adj_rank = it->first;
      i_b = it->second;
      //push back send and receive lists
      vector<Photon> empty_phtn_vec;
      send_list.push_back(empty_phtn_vec);
      phtn_recv_request[i_b] = 
        world.irecv(adj_rank, photon_tag, phtn_recv_buffer[i_b].get_buffer());
      n_receives_posted++;
      phtn_recv_buffer[i_b].set_awaiting();
    } // end loop over adjacent processors
  }

  ////////////////////////////////////////////////////////////////////////
  // main transport loop
  ////////////////////////////////////////////////////////////////////////

  vector<Photon> census_list; //!< End of timestep census list
  stack<Photon> phtn_recv_stack; //!< Stack of received photons

  int send_rank;
  uint64_t tree_count = 0; //!< Total for this node and all children
  uint64_t parent_count = 0;//!< Total complete from the parent node
  uint64_t c1_count = 0; //!< Total complete from child1 subtree
  uint64_t c2_count = 0; //!< Total complete from child2 subtree
  uint64_t n_complete = 0; //!< Completed histories, regardless of origin
  uint64_t n_local_sourced = 0; //!< Photons pulled from source object
  bool finished = false;
  bool from_receive_stack = false;
  Photon phtn;
  event_type event;

  // Number of particles to run between MPI communication 
  const uint32_t batch_size = imc_parameters->get_batch_size();
  // Preferred size of MPI message
  const uint32_t max_buffer_size 
    = imc_parameters->get_particle_message_size();

  while (!finished) {

    uint32_t n = batch_size;
    
    ////////////////////////////////////////////////////////////////////////////
    // Transport photons from source and received list
    ////////////////////////////////////////////////////////////////////////////
    //first, try to transport photons from the received list
    while (n && (!phtn_recv_stack.empty() || (n_local_sourced < n_local))) {
      
      if (!phtn_recv_stack.empty()) {
        phtn = phtn_recv_stack.top();
        from_receive_stack=true;
      }
      else {
        phtn =source.get_photon(rng, dt); 
        n_local_sourced++;
        from_receive_stack=false;
      }

      event = transport_photon_particle_pass(phtn, mesh, rng, next_dt, exit_E,
                                            census_E, rank_abs_E);
      switch(event) {
        // this case should never be reached
        case WAIT:
          break;
        case KILL: 
          n_complete++;
          break;
        case EXIT:
          n_complete++;
          break;
        case CENSUS:
          census_list.push_back(phtn);
          n_complete++;
          break;
        case PASS:
          send_rank = mesh->get_rank(phtn.get_cell());
          int i_b = adjacent_procs[send_rank];
          send_list[i_b].push_back(phtn);
          break;
      }
      n--;
      if (from_receive_stack) phtn_recv_stack.pop();
    }

    ////////////////////////////////////////////////////////////////////////////
    // process photon send and receives 
    ////////////////////////////////////////////////////////////////////////////
    {
      uint32_t i_b; // buffer index
      int adj_rank; // adjacent rank
      for ( std::map<uint32_t, uint32_t>::iterator it=adjacent_procs.begin(); 
        it != adjacent_procs.end(); ++it) {
        adj_rank = it->first;
        i_b = it->second;
        // process send buffer
        if (phtn_send_buffer[i_b].sent()) {
          if (phtn_send_request[i_b].test()) {
            phtn_send_buffer[i_b].reset();
            n_sends_completed++;
          } 
        }

        if ( (phtn_send_buffer[i_b].empty() && !send_list[i_b].empty()) &&
          (send_list[i_b].size() >= max_buffer_size || n_local_sourced == n_local) ) {
          uint32_t n_photons_to_send = max_buffer_size;
          if ( send_list[i_b].size() < max_buffer_size) 
            n_photons_to_send = send_list[i_b].size();
          vector<Photon>::iterator copy_start = send_list[i_b].begin();
          vector<Photon>::iterator copy_end = send_list[i_b].begin()+n_photons_to_send;
          vector<Photon> send_now_list(copy_start, copy_end);
          send_list[i_b].erase(copy_start,copy_end); 
          phtn_send_buffer[i_b].fill(send_now_list);
          n_photons_sent += n_photons_to_send;
          phtn_send_request[i_b] = 
            world.isend(adj_rank, photon_tag, phtn_send_buffer[i_b].get_buffer());
          n_sends_posted++;
          phtn_send_buffer[i_b].set_sent();
          n_photon_messages++;
        }

        //process receive buffer
        if (phtn_recv_buffer[i_b].awaiting()) {
          if (phtn_recv_request[i_b].test()) {
            n_receives_completed++;
            vector<Photon> receive_list = phtn_recv_buffer[i_b].get_buffer();
            for (uint32_t i=0; i<receive_list.size(); i++) 
              phtn_recv_stack.push(receive_list[i]);
            phtn_recv_buffer[i_b].reset();
            //post receive again
            phtn_recv_request[i_b] = world.irecv(adj_rank, 
              photon_tag, 
              phtn_recv_buffer[i_b].get_buffer());
            n_receives_posted++;
            phtn_recv_buffer[i_b].set_awaiting();
          }
        }
      } // end loop over adjacent processors
    }

    ////////////////////////////////////////////////////////////////////////////
    // binary tree completion communication
    ////////////////////////////////////////////////////////////////////////////
    // The number of completed particles is sent up the chain and then reset.
    // This allows us to send the completed count up the tree without
    // trying to synchronize completion from both children. The root
    // never resets the tree count

    //test receives from children and add work to tree count
    if (c1_recv_buffer.awaiting()) {
      if (c1_recv_request.test()) {
        n_receives_completed++;
        c1_recv_buffer.set_received();
        c1_count = c1_recv_buffer.get_buffer()[0];
        //update tree count 
        tree_count+=c1_count;
        //post receive again
        c1_recv_buffer.reset();
        c1_recv_request = world.irecv(child1, count_tag, c1_recv_buffer.get_buffer());
        n_receives_posted++;
        c1_recv_buffer.set_awaiting();
      }
    }
    if (c2_recv_buffer.awaiting()) {
      if (c2_recv_request.test()) {
        n_receives_completed++;
        c2_recv_buffer.set_received();
        c2_count = c2_recv_buffer.get_buffer()[0];
        //update tree count 
        tree_count+=c2_count;
        //post receive again
        c2_recv_buffer.reset();
        c2_recv_request = world.irecv(child2, count_tag, c2_recv_buffer.get_buffer());
        n_receives_posted++;
        c2_recv_buffer.set_awaiting();
      }
    }

    // test receive from parent and add work to tree count
    if (p_recv_buffer.awaiting()) {
      if (p_recv_request.test()) {
        n_receives_completed++;
        p_recv_buffer.set_received();
        parent_count = p_recv_buffer.get_buffer()[0];
      }
    }

    // test sends to parent
    if (p_send_buffer.sent() ) {
      if (p_send_request.test()) {
        n_sends_completed++;
        p_send_buffer.reset();
      }
    }

    // add completed particles from this rank to tree count and reset 
    tree_count+=n_complete;
    n_complete = 0;

    // If tree count is non-zero, buffers are empty, and local work is done 
    // send message to parent. You may still get more work done and send again.
    // That's OK. To finish transport all particles histories must be completed,
    // meaning that all of the sends will be processed (received)
    if ((parent!=proc_null && tree_count) && (n_local==n_local_sourced && phtn_recv_stack.empty()) ) {
      p_send_buffer.fill(vector<uint64_t> (1,tree_count));
      p_send_request = world.isend(parent, count_tag, p_send_buffer.get_buffer());
      n_sends_posted++;
      p_send_buffer.set_sent();
      //reset tree count so work is not double counted
      tree_count =0;
    }

    // If finished, set flag. Otherwise, continue tree messaging
    if (tree_count == n_global || parent_count == n_global) finished = true;
  } // end while

  //send finished count down tree to children and wait for completion
  if (child1 != proc_null) { 
    if (c1_send_buffer.sent()) {
      c1_send_request.wait();
      n_sends_completed++;
    }
    c1_send_buffer.fill(vector<uint64_t> (1,n_global));
    c1_send_request = world.isend(child1, count_tag, c1_send_buffer.get_buffer());
    n_sends_posted++;
    c1_send_request.wait();
    n_sends_completed++;
  }
  if (child2 != proc_null)  {
    if (c2_send_buffer.sent()) {
      c2_send_request.wait();
      n_sends_completed++;
    }
    c2_send_buffer.fill(vector<uint64_t> (1,n_global));
    c2_send_request = world.isend(child2, count_tag, c2_send_buffer.get_buffer());
    n_sends_posted++;
    c2_send_request.wait();
    n_sends_completed++;
  }

  // wait for parent send to complete, if sent
  if (p_send_buffer.sent()) { 
    p_send_request.wait();
    n_sends_completed++;
  }

  // wait for all ranks to finish then send empty photon messages.
  // Do this because it's possible for a rank to receive the empty message
  // while it's still in the transport loop. In that case, it will post a 
  // receive again, which will never have a matching send
  MPI::COMM_WORLD.Barrier();

  //finish off parent's receive call with empty send
  if (parent!=proc_null) {
    p_send_buffer.fill(vector<uint64_t> (1,1));
    p_send_request = world.isend(parent, count_tag, p_send_buffer.get_buffer());
    n_sends_posted++;
    p_send_request.wait();
    n_sends_completed++;
  }
  if (child1 != proc_null) {
    c1_recv_request.wait();
    n_receives_completed++; 
  }
  if (child2 != proc_null) {
    c2_recv_request.wait();
    n_receives_completed++; 
  }
  

  //finish off posted photon receives
  {
    vector<Photon> empty_buffer;
    uint32_t i_b; // buffer index
    int adj_rank; // adjacent rank
    for ( std::map<uint32_t, uint32_t>::iterator it=adjacent_procs.begin(); 
      it != adjacent_procs.end(); ++it) {
      adj_rank = it->first;
      i_b = it->second;
      //wait for completion of previous sends
      if (phtn_send_buffer[i_b].sent()) phtn_send_request[i_b].wait();
      //send empty buffer to finish off receives
      phtn_send_request[i_b] = world.isend(adj_rank, photon_tag, empty_buffer);
      n_sends_posted++;
      phtn_send_request[i_b].wait();
      n_sends_completed++;
    } // end loop over adjacent processors
  }

  // Wait for receive requests
  for (uint32_t i_b=0; i_b<n_adjacent; i_b++) {
    phtn_recv_request[i_b].wait();
    n_receives_completed++;
  }

  MPI::COMM_WORLD.Barrier();

  cout.flush();
  std::sort(census_list.begin(), census_list.end());
  //All ranks have now finished transport
  delete[] phtn_recv_request;
  delete[] phtn_send_request;

  imc_state->set_exit_E(exit_E);
  imc_state->set_post_census_E(census_E);
  imc_state->set_census_size(census_list.size());
  //set diagnostic
  imc_state->set_step_particle_messages(n_photon_messages);
  imc_state->set_step_particles_sent(n_photons_sent);
  imc_state->set_step_sends_posted(n_sends_posted);
  imc_state->set_step_sends_completed(n_sends_completed);
  imc_state->set_step_receives_posted(n_receives_posted);
  imc_state->set_step_receives_completed(n_receives_completed);

  return census_list;
}

#endif // def transport_particle_pass_h_
