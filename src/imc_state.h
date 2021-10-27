//----------------------------------*-C++-*----------------------------------//
/*!
 * \file   imc_state.h
 * \author Alex Long
 * \date   July 24 2014
 * \brief  Holds and prints diagnostic state data
 * \note   Copyright (C) 2017 Los Alamos National Security, LLC.
 *         All rights reserved
 */
//---------------------------------------------------------------------------//

#ifndef imc_state_h_
#define imc_state_h_

#include <cmath>
#include <functional>
#include <iostream>
#include <mpi.h>
#include <vector>

#include "RNG.h"
#include "constants.h"
#include "input.h"
#include "photon.h"

//==============================================================================
/*!
 * \class IMC_State
 * \brief Holds high level information and diagnostic data
 *
 * Keeps track of the simulation time, step, particle counts and
 * energy conservation qualities. Also holds the RNG pointer.
 * \example no test yet
 */
//==============================================================================
class IMC_State {
public:
  //! constructor
  IMC_State(const Input &input)
      : m_dt(input.get_dt()), m_time(input.get_time_start()),
        m_time_stop(input.get_time_finish()), m_step(1),
        m_dt_mult(input.get_time_mult()), m_dt_max(input.get_dt_max()) {
    pre_census_E = 0.0;
    post_census_E = 0.0;
    pre_mat_E = 0.0;
    post_mat_E = 0.0;
    emission_E = 0.0;
    exit_E = 0.0;
    absorbed_E = 0.0;
    source_E = 0.0;

    // 64 bit
    trans_particles = 0;
    census_size = 0;

    m_RNG = new RNG();
    m_RNG->set_seed(input.get_rng_seed() + 4106);

    rank_transport_runtime = 0.0;
  }

  //! Destructor
  ~IMC_State() { delete m_RNG; }

  //--------------------------------------------------------------------------//
  // const functions                                                          //
  //--------------------------------------------------------------------------//

  //! Get current simulation time
  double get_time(void) const { return m_time; }

  //! Get current simulation timestep
  double get_dt(void) const { return m_dt; }

  //! Get current step
  uint32_t get_step(void) const { return m_step; }

  //! Get transported particles for current timestep
  uint64_t get_transported_particles(void) const { return trans_particles; }

  //! Get number of particles in census
  uint64_t get_census_size(void) const { return census_size; }

  //! Get census energy at the beginning of timestep
  double get_pre_census_E(void) { return pre_census_E; }

  //! Get emission energy for current timestep
  double get_emission_E(void) { return emission_E; }

  //! Get next timestep size
  double get_next_dt(void) const {
    double next_dt;
    // multiply by dt_mult if it's going to be less than dt_max
    // other set to dt_max
    if (m_dt * m_dt_mult < m_dt_max)
      next_dt = m_dt * m_dt_mult;
    else
      next_dt = m_dt_max;
    // don't overrun the end time
    if (m_time + next_dt > m_time_stop)
      next_dt = m_time_stop - m_time;

    return next_dt;
  }

  //! Check to see if simulation has completed
  bool finished(void) const {
    using std::abs;
    if (abs(m_time - m_time_stop) < 1.0e-8)
      return true;
    else
      return false;
  }

  //! Print beginning of timestep information
  void print_timestep_header(void) const {
    using std::cout;
    using std::endl;
    cout << "****************************************";
    cout << "****************************************" << endl;
    cout << "Step: " << m_step << "  Start Time: " << m_time << "  End Time: ";
    cout << m_time + m_dt << "  dt: " << m_dt << endl;
  }

  //! Get transport time for this rank on current timestep
  double get_rank_transport_runtime(void) { return rank_transport_runtime; }

  //--------------------------------------------------------------------------//
  // non-const functions                                                      //
  //--------------------------------------------------------------------------//

  //! Perform reduction on diagnostic and conservation quantities and print
  void print_conservation() {
    using std::cout;
    using std::endl;
    using std::plus;

    // define global value
    double g_absorbed_E = 0.0;
    double g_emission_E = 0.0;
    double g_pre_census_E = 0.0;
    double g_pre_mat_E = 0.0;
    double g_post_census_E = 0.0;
    double g_post_mat_E = 0.0;
    double g_exit_E = 0.0;
    // timing values
    double max_transport_time = 0.0;
    double min_transport_time = 0.0;
    // 64 bit global integers
    uint64_t g_census_size = 0;
    uint64_t g_trans_particles = 0;

    // reduce energy conservation values (double)
    MPI_Allreduce(&absorbed_E, &g_absorbed_E, 1, MPI_DOUBLE, MPI_SUM,
                  MPI_COMM_WORLD);
    MPI_Allreduce(&emission_E, &g_emission_E, 1, MPI_DOUBLE, MPI_SUM,
                  MPI_COMM_WORLD);
    MPI_Allreduce(&pre_census_E, &g_pre_census_E, 1, MPI_DOUBLE, MPI_SUM,
                  MPI_COMM_WORLD);
    MPI_Allreduce(&pre_mat_E, &g_pre_mat_E, 1, MPI_DOUBLE, MPI_SUM,
                  MPI_COMM_WORLD);
    MPI_Allreduce(&post_census_E, &g_post_census_E, 1, MPI_DOUBLE, MPI_SUM,
                  MPI_COMM_WORLD);
    MPI_Allreduce(&post_mat_E, &g_post_mat_E, 1, MPI_DOUBLE, MPI_SUM,
                  MPI_COMM_WORLD);
    MPI_Allreduce(&exit_E, &g_exit_E, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    // reduce timestep values
    MPI_Allreduce(&rank_transport_runtime, &max_transport_time, 1, MPI_DOUBLE,
                  MPI_MAX, MPI_COMM_WORLD);
    MPI_Allreduce(&rank_transport_runtime, &min_transport_time, 1, MPI_DOUBLE,
                  MPI_MIN, MPI_COMM_WORLD);

    // reduce diagnostic values
    // 64 bit integer reductions
    MPI_Allreduce(&trans_particles, &g_trans_particles, 1, MPI_UNSIGNED_LONG,
                  MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(&census_size, &g_census_size, 1, MPI_UNSIGNED_LONG, MPI_SUM,
                  MPI_COMM_WORLD);

    double rad_conservation = (g_absorbed_E + g_post_census_E + g_exit_E) -
                              (g_pre_census_E + g_emission_E + source_E);

    double mat_conservation =
        g_post_mat_E - (g_pre_mat_E + g_absorbed_E - g_emission_E);

    cout << "Total Photons transported: " << g_trans_particles << endl;
    cout << "Emission E: " << g_emission_E
         << ", Absorption E: " << g_absorbed_E;
    cout << ", Exit E: " << g_exit_E << endl;
    cout << "Pre census E: " << g_pre_census_E << " Post census E: ";
    cout << g_post_census_E << " Post census Size: " << g_census_size << endl;
    cout << "Pre mat E: " << g_pre_mat_E << " Post mat E: " << g_post_mat_E
         << endl;
    cout << "Radiation conservation: " << rad_conservation << endl;
    cout << "Material conservation: " << mat_conservation << endl;
    cout << "Transport time max/min: " << max_transport_time << "/";
    cout << min_transport_time << endl;
  }

  //! Return random number generator
  RNG *get_rng(void) { return m_RNG; }

  //! Increment time and step counter
  void next_time_step(void) {
    m_time += m_dt;
    m_dt = get_next_dt();
    m_step++;
  }

  //! Set pre-transport census energy  (diagnostic)
  void set_pre_census_E(double _pre_census_E) { pre_census_E = _pre_census_E; }

  //! Set post-transport census energy (diagnostic)
  void set_post_census_E(double _post_census_E) {
    post_census_E = _post_census_E;
  }

  //! Set pre-transport material energy (diagnostic)
  void set_pre_mat_E(double _pre_mat_E) { pre_mat_E = _pre_mat_E; }

  //! Set post-transport material energy (diagnostic)
  void set_post_mat_E(double _post_mat_E) { post_mat_E = _post_mat_E; }

  //! Set timestep emission energy (diagnostic)
  void set_emission_E(double _emission_E) { emission_E = _emission_E; }

  //! Set source energy for current timestep (diagnostic)
  void set_source_E(double _source_E) { source_E = _source_E; }

  //! Set absorbed energy for current timestep (diagnostic)
  void set_absorbed_E(double _absorbed_E) { absorbed_E = _absorbed_E; }

  //! Set exit energy from transport (diagnostic)
  void set_exit_E(double _exit_E) { exit_E = _exit_E; }

  //! set particles transported for current timestep (diagnostic, 64 bit)
  void set_transported_particles(uint64_t _trans_particles) {
    trans_particles = _trans_particles;
  }

  //! Set number of census particles for current timestep (diagnostic, 64 bit)
  void set_census_size(uint64_t _census_size) { census_size = _census_size; }

  //! Set transport runtime for this timestep
  void set_rank_transport_runtime(double _rank_transport_runtime) {
    rank_transport_runtime = _rank_transport_runtime;
  }

  //--------------------------------------------------------------------------//
  // member data                                                              //
  //--------------------------------------------------------------------------//
private:
  // time
  double m_dt;        //!< Current time step size (sh)
  double m_time;      //!< Current time (sh)
  double m_time_stop; //!< End time (sh)
  uint32_t m_step;    //!< Time step (start at 1)
  double m_dt_mult;   //!< Time step multiplier
  double m_dt_max;    //!< Max time step

  // conservation
  double pre_census_E;  //!< Census energy at the beginning of the timestep
  double post_census_E; //!< Census energy at the end of the timestep
  double pre_mat_E;     //!< Material energy at the beginning of timestep
  double post_mat_E;    //!< Material energy at the end of timestep
  double emission_E;    //!< Energy emitted this timestep
  double exit_E;        //!< Energy exiting problem
  double absorbed_E;    //!< Total absorbed energy
  double source_E;      //!< Sourced energy

  // diagnostic 64 bit integers relating to particle and cell counts
  uint64_t trans_particles; //!< Particles transported
  uint64_t census_size;     //!< Number of particles in census

  double rank_transport_runtime; //!< Transport step runtime for this rank

  RNG *m_RNG; //!< Rank specific RNG
};

#endif // imc_state_h_
//---------------------------------------------------------------------------//
// end of imc_state.h
//---------------------------------------------------------------------------//
