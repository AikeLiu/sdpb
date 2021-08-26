#include "../sdp_read.hxx"
#include "../sdp_convert/write_vector.hxx"
#include "../Mesh.hxx"
#include "../max_normalization_index.hxx"
#include "../fill_weights.hxx"

El::BigFloat
eval_summed(const std::vector<std::vector<Polynomial>> &summed_polynomials,
            const El::BigFloat &x);

std::vector<El::BigFloat>
get_zeros(const Mesh &mesh, const El::BigFloat &threshold);

std::vector<std::vector<El::BigFloat>>
compute_spectrum_pvm(const El::Matrix<El::BigFloat> &y,
                     const std::vector<Polynomial_Vector_Matrix> &matrices,
                     const El::BigFloat &threshold)
{
  // pvm2sdp implicitly uses the first element as the normalized column
  std::vector<El::BigFloat> normalization(y.Height() + 1, 0);
  normalization.at(0) = 1;
  const size_t max_index(0);
  std::vector<El::BigFloat> weights(normalization.size());
  fill_weights(y, max_index, normalization, weights);

  const El::BigFloat zero(0);
  std::vector<std::vector<El::BigFloat>> zeros(matrices.size());
  const size_t rank(El::mpi::Rank()), num_procs(El::mpi::Size());
  for(size_t block_index(rank); block_index < matrices.size();
      block_index += num_procs)
    {
      auto &block(matrices[block_index]);
      const size_t max_number_terms([&block]() {
        size_t max(0);
        for(auto &element : block.elements)
          for(auto &poly : element)
            {
              max = std::max(max, poly.coefficients.size());
            }
        return max;
      }());

      // The factor of 6 comes from the limiting scale for Laguerre
      // polynomial roots.
      const El::BigFloat max_delta(6 * max_number_terms);

      const size_t num_rows(block.rows), num_columns(block.cols);

      std::vector<std::vector<Polynomial>> summed_polynomials(num_rows);
      El::BigFloat block_scale(0), product;
      for(size_t row(0); row != num_rows; ++row)
        {
          summed_polynomials[row].reserve(num_columns);
          for(size_t column(0); column != num_columns; ++column)
            {
              auto &summed(summed_polynomials[row].emplace_back());
              summed.coefficients.resize(max_number_terms, zero);
              for(size_t dual_index(0);
                  dual_index != block.elt(row, column).size(); ++dual_index)
                {
                  auto &poly(block.elt(row, column)[dual_index]);
                  for(size_t coeff(0); coeff != poly.coefficients.size();
                      ++coeff)
                    {
                      product = weights[dual_index] * poly.coefficients[coeff];
                      block_scale = std::max(block_scale, El::Abs(product));
                      summed.coefficients[coeff] += product;
                    }
                }
            }
        }
      const El::BigFloat block_epsilon(block_scale
                                       * El::limits::Epsilon<El::BigFloat>());

      // 1/128 should be a small enough relative error so that we are
      // in the regime of convergence.  Then the error estimates will
      // work
      Mesh mesh(
        zero, max_delta,
        [&](const El::BigFloat &x) {
          return eval_summed(summed_polynomials, x);
        },
        (1.0 / 128), block_epsilon);
      zeros.at(block_index) = get_zeros(mesh, threshold);
    }
  return zeros;
}