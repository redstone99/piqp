// This file is part of PIQP.
//
// Copyright (c) 2023 EPFL
// Copyright (c) 2022 INRIA
//
// This source code is licensed under the BSD 2-Clause License found in the
// LICENSE file in the root directory of this source tree.

#define PIQP_EIGEN_CHECK_MALLOC

#include "piqp/piqp.hpp"
#include "piqp/utils/random.hpp"
#include "piqp/utils/sparse.hpp"

#include "gtest/gtest.h"
#include "utils.hpp"

using namespace piqp;

using T = double;
using I = int;

TEST(Ordering, Natural)
{
    isize dim = 10;
    T sparsity_factor = 0.5;

    SparseMat<T, I> A = rand::sparse_positive_definite_upper_triangular_rand<T, I>(dim, sparsity_factor);

    NaturalOrdering<I> ordering;
    ordering.init(A);

    SparseMat<T, I> C;
    Vec<I> Ai_to_Ci = permute_sparse_symmetric_matrix(A, C, ordering);

    assert_sparse_matrices_equal(A, C);

    Vec<I> Ai_to_Ci_expect(Ai_to_Ci.rows());
    for (isize i = 0; i < Ai_to_Ci.rows(); i++)
    {
        Ai_to_Ci_expect(i) = i;
    }
    ASSERT_EQ(Ai_to_Ci, Ai_to_Ci_expect);
}

TEST(Ordering, AMD)
{
    // 1 0 2 3
    // 0 4 0 5
    // 0 0 6 0
    // 0 0 0 7
    SparseMat<T, I> A(4, 4);
    std::vector<Eigen::Triplet<T, I>> A_triplets = {{0,0,1}, {0,2,2}, {0,3,3}, {1,1,4}, {1,3,5}, {2,2,6}, {3,3,7}};
    A.setFromTriplets(A_triplets.begin(), A_triplets.end());
    A.makeCompressed();

    AMDOrdering<I> ordering;
    ordering.init(A);
    // ordering is [1 2 0 3]

    SparseMat<T, I> C;
    Vec<I> Ai_to_Ci = permute_sparse_symmetric_matrix(A, C, ordering);

    SparseMat<T, I> C_expect(4, 4);
    std::vector<Eigen::Triplet<T, I>> C_triplets = {{0,0,4}, {0,3,5}, {1,1,6}, {1,2,2}, {2,2,1}, {2,3,3}, {3,3,7}};
    C_expect.setFromTriplets(C_triplets.begin(), C_triplets.end());
    C_expect.makeCompressed();

    assert_sparse_matrices_equal(C, C_expect);

    Vec<I> Ai_to_Ci_expect(7);
    Ai_to_Ci_expect << 3, 0, 2, 1, 5, 4, 6;
    ASSERT_EQ(Ai_to_Ci, Ai_to_Ci_expect);
}