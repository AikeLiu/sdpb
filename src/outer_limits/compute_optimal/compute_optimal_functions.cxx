#include "Mesh.hxx"

#include "../Function.hxx"
#include "../../sdp_solve.hxx"
#include "../../ostream_set.hxx"
#include "../../ostream_vector.hxx"
#include "../../set_stream_precision.hxx"

void compute_y_transform(
  const std::vector<std::vector<std::vector<std::vector<Function>>>>
    &function_blocks,
  const std::vector<std::set<El::BigFloat>> &points,
  const std::vector<El::BigFloat> &objectives,
  const std::vector<El::BigFloat> &normalization,
  const SDP_Solver_Parameters &parameters, const size_t &max_index,
  const El::Grid &global_grid,
  El::DistMatrix<El::BigFloat, El::STAR, El::STAR> &yp_to_y,
  El::DistMatrix<El::BigFloat, El::STAR, El::STAR> &y_to_yp_star,
  El::DistMatrix<El::BigFloat, El::STAR, El::STAR> &dual_objective_b_star,
  El::BigFloat &b_scale, El::BigFloat &primal_c_scale);

void setup_constraints_functions(
  const size_t &max_index, const size_t &num_blocks,
  const El::BigFloat &infinity,
  const std::vector<std::vector<std::vector<std::vector<Function>>>>
    &function_blocks,
  const std::vector<El::BigFloat> &normalization,
  const std::vector<std::set<El::BigFloat>> &points,
  std::vector<std::vector<El::BigFloat>> &primal_objective_c,
  std::vector<El::Matrix<El::BigFloat>> &free_var_matrix);

El::BigFloat eval_weighted_functions(
  const El::BigFloat &infinity,
  const std::vector<std::vector<std::vector<Function>>> &function_blocks,
  const El::BigFloat &x, const std::vector<El::BigFloat> &weights);

std::vector<El::BigFloat>
get_new_points(const Mesh &mesh, const El::BigFloat &block_epsilon);

std::vector<El::BigFloat> compute_optimal_functions(
  const std::vector<std::vector<std::vector<std::vector<Function>>>>
    &function_blocks,
  const std::vector<std::vector<El::BigFloat>> &initial_points,
  const std::vector<El::BigFloat> &objectives,
  const std::vector<El::BigFloat> &normalization,
  const SDP_Solver_Parameters &parameters_in)
{
  if(initial_points.size() != function_blocks.size())
    {
      throw std::runtime_error(
        "Size are different: Positive_Matrix_With_Prefactor: "
        + std::to_string(function_blocks.size())
        + ", initial points: " + std::to_string(initial_points.size()));
    }
  SDP_Solver_Parameters parameters(parameters_in);

  const size_t rank(El::mpi::Rank()), num_procs(El::mpi::Size()),
    num_weights(normalization.size());

  const size_t num_blocks(initial_points.size());
  std::vector<El::BigFloat> weights(num_weights, 0);
  std::vector<std::set<El::BigFloat>> points(num_blocks);
  std::vector<std::vector<El::BigFloat>> new_points(num_blocks);

  // GMP does not have a special infinity value, so we use max double.
  const El::BigFloat infinity(std::numeric_limits<double>::max());
  // Use the input points and add inifinty
  for(size_t block(0); block < num_blocks; ++block)
    {
      for(auto &point : initial_points.at(block))
        {
          points.at(block).emplace(point);
        }
      points.at(block).emplace(infinity);
    }

  // TODO: This is duplicated from sdp2input/write_output/write_output.cxx
  size_t max_index(0);
  El::BigFloat max_normalization(0);
  for(size_t index(0); index != normalization.size(); ++index)
    {
      const El::BigFloat element(Abs(normalization[index]));
      if(element > max_normalization)
        {
          max_normalization = element;
          max_index = index;
        }
    }

  const El::Grid global_grid;
  El::DistMatrix<El::BigFloat, El::STAR, El::STAR> yp_to_y_star(global_grid),
    y_to_yp_star(global_grid), dual_objective_b_star(global_grid);
  El::BigFloat b_scale, primal_c_scale;
  compute_y_transform(function_blocks, points, objectives, normalization,
                      parameters, max_index, global_grid, yp_to_y_star,
                      y_to_yp_star, dual_objective_b_star, b_scale,
                      primal_c_scale);
  parameters.duality_gap_threshold = 1.1;
  while(parameters.duality_gap_threshold > parameters_in.duality_gap_threshold)
    {
      size_t num_constraints(0);
      std::vector<size_t> matrix_dimensions;
      for(size_t block(0); block != num_blocks; ++block)
        {
          for(auto &point : new_points.at(block))
            {
              points.at(block).emplace(point);
            }
          num_constraints += points.at(block).size();
          matrix_dimensions.insert(matrix_dimensions.end(),
                                   points.at(block).size(),
                                   function_blocks[block].size());
          if(rank == 0 && parameters.verbosity >= Verbosity::debug)
            {
              std::cout << "points: " << block << " " << points.at(block)
                        << "\n";
            }
        }

      if(rank == 0)
        {
          std::cout << "num_constraints: " << num_constraints << "\n";
        }

      std::vector<std::vector<El::BigFloat>> primal_objective_c;
      primal_objective_c.reserve(num_constraints);
      std::vector<El::Matrix<El::BigFloat>> free_var_matrix;
      free_var_matrix.reserve(num_constraints);

      setup_constraints_functions(max_index, num_blocks, infinity,
                                  function_blocks, normalization, points,
                                  primal_objective_c, free_var_matrix);

      const El::BigFloat objective_const(objectives.at(max_index)
                                         / normalization.at(max_index));
      std::vector<El::BigFloat> dual_objective_b;
      dual_objective_b.reserve(normalization.size() - 1);
      for(size_t index = 0; index < normalization.size(); ++index)
        {
          if(index != max_index)
            {
              dual_objective_b.push_back(objectives.at(index)
                                         - normalization.at(index)
                                             * objective_const);
            }
        }

      Block_Info block_info(matrix_dimensions, parameters.procs_per_node,
                            parameters.proc_granularity, parameters.verbosity);
      El::Grid grid(block_info.mpi_comm.value);

      SDP sdp(objective_const, dual_objective_b, primal_objective_c,
              free_var_matrix, yp_to_y_star, y_to_yp_star,
              dual_objective_b_star, b_scale, primal_c_scale, block_info,
              grid);

      SDP_Solver solver(parameters, block_info, grid,
                        sdp.dual_objective_b.Height());

      for(auto &block : solver.y.blocks)
        {
          if(block.GlobalCol(0) == 0)
            {
              for(size_t row(0); row != size_t(block.LocalHeight()); ++row)
                {
                  size_t global_row(block.GlobalRow(row));
                  const size_t index(global_row
                                     + (global_row < max_index ? 0 : 1));
                  block.SetLocal(row, 0, weights.at(index));
                }
            }
        }

      bool has_new_points(false);
      while(!has_new_points
            && parameters.duality_gap_threshold
                 > parameters_in.duality_gap_threshold)
        {
          if(rank == 0)
            {
              std::cout << "Threshold: " << parameters.duality_gap_threshold
                        << "\n";
            }

          Timers timers(parameters.verbosity >= Verbosity::debug);
          SDP_Solver_Terminate_Reason reason
            = solver.run(parameters, block_info, sdp, grid, timers);

          if(rank == 0)
            {
              set_stream_precision(std::cout);
              std::cout << "-----" << reason << "-----\n"
                        << '\n'
                        << "primalObjective = " << solver.primal_objective
                        << '\n'
                        << "dualObjective   = " << solver.dual_objective
                        << '\n'
                        << "dualityGap      = " << solver.duality_gap << '\n'
                        << "primalError     = " << solver.primal_error()
                        << '\n'
                        << "dualError       = " << solver.dual_error << '\n'
                        << '\n';
            }

          if(reason == SDP_Solver_Terminate_Reason::MaxComplementarityExceeded
             || reason == SDP_Solver_Terminate_Reason::MaxIterationsExceeded
             || reason == SDP_Solver_Terminate_Reason::MaxRuntimeExceeded)
            {
              std::stringstream ss;
              ss << "Can not find solution: " << reason;
              throw std::runtime_error(ss.str());
            }

          // y is duplicated among cores, so only need to print out copy on
          // the root node.
          // THe weight at max_index is determined by the normalization
          // condition dot(norm,weights)=1
          El::DistMatrix<El::BigFloat> yp(dual_objective_b.size(), 1,
                                          yp_to_y_star.Grid());
          El::Zero(yp);
          El::DistMatrix<El::BigFloat> y(yp);
          El::DistMatrix<El::BigFloat, El::STAR, El::STAR> yp_star(
            solver.y.blocks.at(0));
          for(int64_t row(0); row != yp.LocalHeight(); ++row)
            {
              int64_t global_row(yp.GlobalRow(row));
              for(int64_t column(0); column != yp.LocalWidth(); ++column)
                {
                  int64_t global_column(yp.GlobalCol(column));
                  yp.SetLocal(row, column,
                              yp_star.GetLocal(global_row, global_column));
                }
            }
          El::Gemv(El::Orientation::NORMAL, El::BigFloat(1.0), yp_to_y_star,
                   yp, El::BigFloat(0.0), y);
          El::DistMatrix<El::BigFloat, El::STAR, El::STAR> y_star(y);

          weights.at(max_index) = 1;
          for(size_t block_row(0); block_row != size_t(y_star.LocalHeight());
              ++block_row)
            {
              const size_t index(block_row + (block_row < max_index ? 0 : 1));
              weights.at(index) = y_star.GetLocalCRef(block_row, 0);
              weights.at(max_index)
                -= weights.at(index) * normalization.at(index);
            }
          weights.at(max_index) /= normalization.at(max_index);
          if(rank == 0)
            {
              std::cout.precision(20);
              std::cout << "weight: " << weights << "\n";

              El::BigFloat optimal(0);
              for(size_t index(0); index < objectives.size(); ++index)
                {
                  optimal += objectives[index] * weights[index];
                }
              std::cout << "optimal: " << optimal << "\n";
            }
          std::vector<size_t> num_new_points(num_blocks, 0);
          for(size_t block(rank); block < num_blocks; block += num_procs)
            {
              // std::cout << "block start: " << num_blocks << " " << rank << "
              // "
              //           << block << "\n";
              // TODO: These can both be precomputed
              El::BigFloat max_delta(infinity), block_scale(0);
              size_t max_degree(0);
              for(auto &row : function_blocks[block])
                for(auto &column : row)
                  for(size_t function_index(0);
                      function_index != column.size(); ++function_index)
                    {
                      auto &f(column[function_index]);
                      max_delta = El::Min(max_delta, f.max_delta);
                      max_degree
                        = std::max(max_degree, f.chebyshev_coeffs.size());
                      for(auto &coeff : f.chebyshev_coeffs)
                        {
                          block_scale = std::max(
                            block_scale,
                            El::Abs(coeff * weights[function_index]));
                        }
                    }

              const El::BigFloat block_epsilon(
                block_scale * El::limits::Epsilon<El::BigFloat>());

              // 1/128 should be a small enough relative error so that we are
              // in the regime of convergence.  Then the error estimates will
              // work

              // std::cout << "block: " << rank << " " << block_epsilon << " "
              //           << function_blocks[block].front().front().size() <<
              //           " "
              //           << weights.size() << "\n";

              // std::cout
              //   << "block mesh: " << num_blocks << " " << rank << " " <<
              //   block
              //   << " " << *(points.at(block).begin()) << " " << max_delta
              //   << " " << block_scale << " "
              //   << block_scale * El::limits::Epsilon<El::BigFloat>()
              //   << " "
              //   << (block_scale * El::limits::Epsilon<El::BigFloat>()
              //       * objectives.size() * max_degree * max_degree)
              //   << " "
              //   // << function_blocks[block].size() << " "
              //   // << function_blocks[block].front().size() << " "
              //   // << function_blocks[block].front().front().size() << " "
              //   // <<
              //   //
              //   function_blocks[block].front().front().front().chebyshev_coeffs.size()
              //   // << " "
              //   << "\n";

              Mesh mesh(
                *(points.at(block).begin()), max_delta,
                [&](const El::BigFloat &x) {
                  return eval_weighted_functions(
                    infinity, function_blocks[block], x, weights);
                },
                (1.0 / 128), block_epsilon);

              // Each term in the polynomial adds degree*epsilon to
              // the error, and degree goes from 0 to max_degree, so
              // the sum is max_degree**2.  There are num_functions of
              // these, so the final multiplicative term is
              // num_functions*max_degree**2.

              // std::cout << "block meshed: "
              //           << num_blocks << " " << rank << " " << block << "\n"
              //           << std::flush;

              std::vector<El::BigFloat> candidates(
                get_new_points(mesh, block_epsilon));

              // std::cout << "block candidates: "
              //           << num_blocks << " " << rank << " " << block << "\n";

              new_points.at(block).clear();
              for(auto &point : candidates)
                {
                  if(points.at(block).count(point) == 0)
                    {
                      new_points.at(block).push_back(point);
                      ++num_new_points.at(block);
                    }
                }
              // std::cout << "block end: "
              //           << num_blocks << " " << rank << " " << block << "\n";
            }
          // std::cout << "meshed: "
          //           << num_blocks << " " << rank << "\n";

          El::mpi::AllReduce(num_new_points.data(), num_new_points.size(),
                             El::mpi::SUM, El::mpi::COMM_WORLD);

          for(size_t block(0); block != num_blocks; ++block)
            {
              new_points.at(block).resize(num_new_points.at(block));
              El::mpi::Broadcast(new_points.at(block).data(),
                                 num_new_points.at(block), block % num_procs,
                                 El::mpi::COMM_WORLD);
            }
          has_new_points
            = (find_if(num_new_points.begin(), num_new_points.end(),
                       [](const size_t &n) { return n != 0; })
               != num_new_points.end());
          if(!has_new_points)
            {
              parameters.duality_gap_threshold *= (1.0 / 8);
            }
          exit(0);
        }
    }
  return weights;
}