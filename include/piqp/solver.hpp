// This file is part of PIQP.
//
// Copyright (c) 2023 EPFL
// Copyright (c) 2022 INRIA
//
// This source code is licensed under the BSD 2-Clause License found in the
// LICENSE file in the root directory of this source tree.

#ifndef PIQP_SOLVER_HPP
#define PIQP_SOLVER_HPP
#include <iostream>
#include <Eigen/Dense>
#include <Eigen/Sparse>

#include "piqp/timer.hpp"
#include "piqp/results.hpp"
#include "piqp/settings.hpp"
#include "piqp/dense/data.hpp"
#include "piqp/dense/preconditioner.hpp"
#include "piqp/dense/kkt.hpp"
#include "piqp/sparse/data.hpp"
#include "piqp/sparse/preconditioner.hpp"
#include "piqp/sparse/kkt.hpp"
#include "piqp/utils/optional.hpp"

namespace piqp
{

enum SolverMatrixType
{
    PIQP_DENSE = 0,
    PIQP_SPARSE = 1
};

template<typename Derived, typename T, typename I, typename Preconditioner, int MatrixType, int Mode = KKTMode::KKT_FULL>
class SolverBase
{
protected:
    using DataType = typename std::conditional<MatrixType == PIQP_DENSE, dense::Data<T>, sparse::Data<T, I>>::type;
    using KKTType = typename std::conditional<MatrixType == PIQP_DENSE, dense::KKT<T>, sparse::KKT<T, I, Mode>>::type;

    Timer<T> m_timer;
    Result<T> m_result;
    Settings<T> m_settings;
    DataType m_data;
    Preconditioner m_preconditioner;
    KKTType m_kkt;

    bool m_kkt_init_state = false;
    bool m_setup_done = false;

    // residuals
    Vec<T> rx;
    Vec<T> ry;
    Vec<T> rz;
    Vec<T> rz_lb;
    Vec<T> rz_ub;
    Vec<T> rs;
    Vec<T> rs_lb;
    Vec<T> rs_ub;

    // non-regularized residuals
    Vec<T> rx_nr;
    Vec<T> ry_nr;
    Vec<T> rz_nr;
    Vec<T> rz_lb_nr;
    Vec<T> rz_ub_nr;

    // primal and dual steps
    Vec<T> dx;
    Vec<T> dy;
    Vec<T> dz;
    Vec<T> dz_lb;
    Vec<T> dz_ub;
    Vec<T> ds;
    Vec<T> ds_lb;
    Vec<T> ds_ub;

    T primal_rel_inf;
    T dual_rel_inf;

public:
    SolverBase() : m_kkt(m_data) {};

    Settings<T>& settings() { return m_settings; }

    const Result<T>& result() const { return m_result; }

    Status solve()
    {
        if (m_settings.verbose)
        {
            printf("----------------------------------------------------------\n");
            printf("                           PIQP                           \n");
            printf("           (c) Roland Schwan, Colin N. Jones              \n");
            printf("   École Polytechnique Fédérale de Lausanne (EPFL) 2023   \n");
            printf("----------------------------------------------------------\n");
            if (MatrixType == PIQP_DENSE)
            {
                printf("variables n = %ld\n", m_data.n);
                printf("equality constraints p = %ld\n", m_data.p);
                printf("inequality constraints m = %ld\n", m_data.m);
            }
            else
            {
                printf("variables n = %ld, nzz(P upper triangular) = %ld\n", m_data.n, m_data.non_zeros_P_utri());
                printf("equality constraints p = %ld, nnz(A) = %ld\n", m_data.p, m_data.non_zeros_A());
                printf("inequality constraints m = %ld, nnz(G) = %ld\n", m_data.m, m_data.non_zeros_G());
            }
            printf("variable lower bounds n_lb = %ld\n", m_data.n_lb);
            printf("variable upper bounds n_ub = %ld\n", m_data.n_ub);
            printf("\n");
            printf("iter  prim_cost      dual_cost      prim_inf      dual_inf      rho         delta       mu          prim_step   dual_step\n");
        }

        if (m_settings.compute_timings)
        {
            m_timer.start();
        }

        Status status = solve_impl();

        unscale_results();
        restore_box_dual();

        if (m_settings.compute_timings)
        {
            T solve_time = m_timer.stop();
            m_result.info.solve_time = solve_time;
            m_result.info.run_time += solve_time;
        }

        if (m_settings.verbose)
        {
            printf("\n");
            printf("status:               %s\n", status_to_string(status));
            printf("number of iterations: %ld\n", m_result.info.iter);
            if (m_settings.compute_timings)
            {
                printf("total run time:       %.3es\n", m_result.info.run_time);
                printf("  setup time:         %.3es\n", m_result.info.setup_time);
                printf("  update time:        %.3es\n", m_result.info.update_time);
                printf("  solve time:         %.3es\n", m_result.info.solve_time);
            }
        }

        return status;
    }

protected:
    template<typename MatType>
    void setup_impl(const MatType& P,
                    const CVecRef<T>& c,
                    const MatType& A,
                    const CVecRef<T>& b,
                    const MatType& G,
                    const CVecRef<T>& h,
                    const optional<CVecRef<T>> x_lb,
                    const optional<CVecRef<T>> x_ub)
    {
        if (m_settings.compute_timings)
        {
            m_timer.start();
        }

        m_data.n = P.rows();
        m_data.p = A.rows();
        m_data.m = G.rows();

        eigen_assert(P.rows() == m_data.n && P.cols() == m_data.n && "P must be square");
        eigen_assert(A.rows() == m_data.p && A.cols() == m_data.n && "A must have correct dimensions");
        eigen_assert(G.rows() == m_data.m && G.cols() == m_data.n && "G must have correct dimensions");
        eigen_assert(c.size() == m_data.n && "c must have correct dimensions");
        eigen_assert(b.size() == m_data.p && "b must have correct dimensions");
        eigen_assert(h.size() == m_data.m && "h must have correct dimensions");
        if (x_lb.has_value()) { eigen_assert(x_lb->size() == m_data.n && "x_lb must have correct dimensions"); }
        if (x_ub.has_value()) { eigen_assert(x_ub->size() == m_data.n && "x_ub must have correct dimensions"); }

        m_data.P_utri = P.template triangularView<Eigen::Upper>();
        m_data.AT = A.transpose();
        m_data.GT = G.transpose();
        m_data.c = c;
        m_data.b = b;
        m_data.h = h;

        m_data.x_lb_n.resize(m_data.n);
        m_data.x_ub.resize(m_data.n);
        m_data.x_lb_idx.resize(m_data.n);
        m_data.x_ub_idx.resize(m_data.n);

        setup_lb_data(x_lb);
        setup_ub_data(x_ub);

        init_workspace();

        m_preconditioner.init(m_data);
        m_preconditioner.scale_data(m_data, false, m_settings.preconditioner_iter);

        m_kkt.init(m_result.info.rho, m_result.info.delta);
        m_kkt_init_state = true;

        m_setup_done = true;

        if (m_settings.compute_timings)
        {
            T setup_time = m_timer.stop();
            m_result.info.setup_time = setup_time;
            m_result.info.run_time += setup_time;
        }
    }

    void setup_lb_data(const optional<CVecRef<T>> x_lb)
    {
        isize n_lb = 0;
        if (x_lb.has_value())
        {
            isize i_lb = 0;
            for (isize i = 0; i < m_data.n; i++)
            {
                if ((*x_lb)(i) > -PIQP_INF)
                {
                    n_lb += 1;
                    m_data.x_lb_n(i_lb) = -(*x_lb)(i);
                    m_data.x_lb_idx(i_lb) = i;
                    i_lb++;
                }
            }
        }
        m_data.n_lb = n_lb;
    }

    void setup_ub_data(const optional<CVecRef<T>> x_ub)
    {
        isize n_ub = 0;
        if (x_ub.has_value())
        {
            isize i_ub = 0;
            for (isize i = 0; i < m_data.n; i++)
            {
                if ((*x_ub)(i) < PIQP_INF)
                {
                    n_ub += 1;
                    m_data.x_ub(i_ub) = (*x_ub)(i);
                    m_data.x_ub_idx(i_ub) = i;
                    i_ub++;
                }
            }
        }
        m_data.n_ub = n_ub;
    }

    void init_workspace()
    {
        // init result
        m_result.x.resize(m_data.n);
        m_result.y.resize(m_data.p);
        m_result.z.resize(m_data.m);
        m_result.z_lb.resize(m_data.n);
        m_result.z_ub.resize(m_data.n);
        m_result.s.resize(m_data.m);
        m_result.s_lb.resize(m_data.n);
        m_result.s_ub.resize(m_data.n);

        m_result.zeta.resize(m_data.n);
        m_result.lambda.resize(m_data.p);
        m_result.nu.resize(m_data.m);
        m_result.nu_lb.resize(m_data.n);
        m_result.nu_ub.resize(m_data.n);

        // init workspace
        m_result.info.rho = m_settings.rho_init;
        m_result.info.delta = m_settings.delta_init;
        m_result.info.setup_time = 0;
        m_result.info.update_time = 0;
        m_result.info.solve_time = 0;
        m_result.info.run_time = 0;

        rx.resize(m_data.n);
        ry.resize(m_data.p);
        rz.resize(m_data.m);
        rz_lb.resize(m_data.n);
        rz_ub.resize(m_data.n);
        rs.resize(m_data.m);
        rs_lb.resize(m_data.n);
        rs_ub.resize(m_data.n);

        rx_nr.resize(m_data.n);
        ry_nr.resize(m_data.p);
        rz_nr.resize(m_data.m);
        rz_lb_nr.resize(m_data.n);
        rz_ub_nr.resize(m_data.n);

        dx.resize(m_data.n);
        dy.resize(m_data.p);
        dz.resize(m_data.m);
        dz_lb.resize(m_data.n);
        dz_ub.resize(m_data.n);
        ds.resize(m_data.m);
        ds_lb.resize(m_data.n);
        ds_ub.resize(m_data.n);
    }

    Status solve_impl()
    {
        auto s_lb = m_result.s_lb.head(m_data.n_lb);
        auto s_ub = m_result.s_ub.head(m_data.n_ub);
        auto z_lb = m_result.z_lb.head(m_data.n_lb);
        auto z_ub = m_result.z_ub.head(m_data.n_ub);
        auto nu_lb = m_result.nu_lb.head(m_data.n_lb);
        auto nu_ub = m_result.nu_ub.head(m_data.n_ub);

        if (!m_setup_done)
        {
            eigen_assert(false && "Solver not setup yet");
            m_result.info.status = Status::PIQP_UNSOLVED;
            return m_result.info.status;
        }

        if (!m_settings.verify_settings())
        {
            m_result.info.status = Status::PIQP_INVALID_SETTINGS;
            return m_result.info.status;
        }

        m_result.info.status = Status::PIQP_UNSOLVED;
        m_result.info.iter = 0;
        m_result.info.reg_limit = m_settings.reg_lower_limit;
        m_result.info.factor_retires = 0;
        m_result.info.no_primal_update = 0;
        m_result.info.no_dual_update = 0;
        m_result.info.mu = 0;
        m_result.info.primal_step = 0;
        m_result.info.dual_step = 0;

        if (!m_kkt_init_state)
        {
            m_result.info.rho = m_settings.rho_init;
            m_result.info.delta = m_settings.delta_init;

            m_result.s.setConstant(1);
            s_lb.head(m_data.n_lb).setConstant(1);
            s_ub.head(m_data.n_ub).setConstant(1);
            m_result.z.setConstant(1);
            z_lb.head(m_data.n_lb).setConstant(1);
            z_ub.head(m_data.n_ub).setConstant(1);
            m_kkt.update_scalings(m_result.info.rho, m_result.info.delta,
                                  m_result.s, m_result.s_lb, m_result.s_ub,
                                  m_result.z, m_result.z_lb, m_result.z_ub);
        }

        while (!m_kkt.factorize())
        {
            if (m_result.info.factor_retires < m_settings.max_factor_retires)
            {
                m_result.info.delta *= 100;
                m_result.info.rho *= 100;
                m_result.info.factor_retires++;
                m_result.info.reg_limit = std::min(10 * m_result.info.reg_limit, m_settings.feas_tol_abs);
            }
            else
            {
                m_result.info.status = Status::PIQP_NUMERICS;
                return m_result.info.status;
            }
        }
        m_result.info.factor_retires = 0;

        rx = -m_data.c;
        // avoid unnecessary copies
        // ry = m_data.b;
        // rz = m_data.h;
        // rz_lb = m_data.x_lb;
        // rz_ub = m_data.x_ub;
        rs.setZero();
        rs_lb.setZero();
        rs_ub.setZero();
        m_kkt.solve(rx, m_data.b,
                    m_data.h, m_data.x_lb_n, m_data.x_ub,
                    rs, rs_lb, rs_ub,
                    m_result.x, m_result.y,
                    m_result.z, m_result.z_lb, m_result.z_ub,
                    m_result.s, m_result.s_lb, m_result.s_ub);

        if (m_data.m + m_data.n_lb + m_data.n_ub > 0)
        {
            T s_norm = T(0);
            s_norm = std::max(s_norm, m_result.s.template lpNorm<Eigen::Infinity>());
            s_norm = std::max(s_norm, s_lb.template lpNorm<Eigen::Infinity>());
            s_norm = std::max(s_norm, s_ub.template lpNorm<Eigen::Infinity>());
            if (s_norm <= 1e-4)
            {
                // 0.1 is arbitrary
                m_result.s.setConstant(0.1);
                s_lb.setConstant(0.1);
                s_ub.setConstant(0.1);
                m_result.z.setConstant(0.1);
                z_lb.setConstant(0.1);
                z_ub.setConstant(0.1);
            }

            T delta_s = T(0);
            if (m_data.m > 0) delta_s = std::max(delta_s, -T(1.5) * m_result.s.minCoeff());
            if (m_data.n_lb > 0) delta_s = std::max(delta_s, -T(1.5) * s_lb.minCoeff());
            if (m_data.n_ub > 0) delta_s = std::max(delta_s, -T(1.5) * s_ub.minCoeff());
            T delta_z = T(0);
            if (m_data.m > 0) delta_z = std::max(delta_z, -T(1.5) * m_result.z.minCoeff());
            if (m_data.n_lb > 0) delta_z = std::max(delta_z, -T(1.5) * z_lb.minCoeff());
            if (m_data.n_ub > 0) delta_z = std::max(delta_z, -T(1.5) * z_ub.minCoeff());
            T tmp_prod = (m_result.s.array() + delta_s).matrix().dot((m_result.z.array() + delta_z).matrix());
            tmp_prod += (s_lb.array() + delta_s).matrix().dot((z_lb.array() + delta_z).matrix());
            tmp_prod += (s_ub.array() + delta_s).matrix().dot((z_ub.array() + delta_z).matrix());
            T delta_s_bar = delta_s + (T(0.5) * tmp_prod) / (m_result.z.sum() + z_lb.sum() + z_ub.sum() + (m_data.m + m_data.n_lb + m_data.n_ub) * delta_z);
            T delta_z_bar = delta_z + (T(0.5) * tmp_prod) / (m_result.s.sum() + s_lb.sum() + s_ub.sum() + (m_data.m + m_data.n_lb + m_data.n_ub) * delta_s);

            m_result.s.array() += delta_s_bar;
            s_lb.array() += delta_s_bar;
            s_ub.array() += delta_s_bar;
            m_result.z.array() += delta_z_bar;
            z_lb.array() += delta_z_bar;
            z_ub.array() += delta_z_bar;

            m_result.info.mu = (m_result.s.dot(m_result.z) + s_lb.dot(z_lb) + s_ub.dot(z_ub) ) / (m_data.m + m_data.n_lb + m_data.n_ub);
        }

        m_result.zeta = m_result.x;
        m_result.lambda = m_result.y;
        m_result.nu = m_result.z;
        nu_lb = z_lb;
        nu_ub = z_ub;

        while (m_result.info.iter < m_settings.max_iter)
        {
            if (m_result.info.iter == 0)
            {
                update_nr_residuals();
            }

            m_result.info.primal_inf = m_preconditioner.unscale_primal_res_eq(ry_nr).template lpNorm<Eigen::Infinity>();
            m_result.info.primal_inf = std::max(m_result.info.primal_inf, m_preconditioner.unscale_primal_res_ineq(rz_nr).template lpNorm<Eigen::Infinity>());
            m_result.info.primal_inf = std::max(m_result.info.primal_inf, m_preconditioner.unscale_primal_res_lb(rz_lb_nr.head(m_data.n_lb)).template lpNorm<Eigen::Infinity>());
            m_result.info.primal_inf = std::max(m_result.info.primal_inf, m_preconditioner.unscale_primal_res_ub(rz_ub_nr.head(m_data.n_ub)).template lpNorm<Eigen::Infinity>());
            m_result.info.dual_inf = m_preconditioner.unscale_dual_res(rx_nr).template lpNorm<Eigen::Infinity>();

            if (m_settings.verbose)
            {
                // use rx as temporary variables
                rx.noalias() = m_data.P_utri * m_result.x;
                rx.noalias() += m_data.P_utri.transpose().template triangularView<Eigen::StrictlyLower>() * m_result.x;
                T xPx_half = T(0.5) * m_result.x.dot(rx);

                T primal_cost = xPx_half + m_data.c.dot(m_result.x);
                T dual_cost = -xPx_half - m_data.b.dot(m_result.y) - m_data.h.dot(m_result.z);
                dual_cost -= m_data.x_lb_n.head(m_data.n_lb).dot(z_lb);
                dual_cost -= m_data.x_ub.head(m_data.n_ub).dot(z_ub);

                primal_cost = m_preconditioner.unscale_cost(primal_cost);
                dual_cost = m_preconditioner.unscale_cost(dual_cost);

                printf("%3ld   % .5e   % .5e   %.5e   %.5e   %.3e   %.3e   %.3e   %.3e   %.3e\n",
                       m_result.info.iter,
                       primal_cost,
                       dual_cost,
                       m_result.info.primal_inf,
                       m_result.info.dual_inf,
                       m_result.info.rho,
                       m_result.info.delta,
                       m_result.info.mu,
                       m_result.info.primal_step,
                       m_result.info.dual_step);
            }

            if (m_result.info.primal_inf < m_settings.feas_tol_abs + m_settings.feas_tol_rel * primal_rel_inf &&
                m_result.info.dual_inf < m_settings.feas_tol_abs + m_settings.feas_tol_rel * dual_rel_inf &&
                m_result.info.mu < m_settings.dual_tol)
            {
                m_result.info.status = Status::PIQP_SOLVED;
                return m_result.info.status;
            }

            rx = rx_nr - m_result.info.rho * (m_result.x - m_result.zeta);
            ry = ry_nr - m_result.info.delta * (m_result.lambda - m_result.y);
            rz = rz_nr - m_result.info.delta * (m_result.nu - m_result.z);
            rz_lb.head(m_data.n_lb) = rz_lb_nr.head(m_data.n_lb) - m_result.info.delta * (nu_lb - z_lb);
            rz_ub.head(m_data.n_ub) = rz_ub_nr.head(m_data.n_ub) - m_result.info.delta * (nu_ub - z_ub);

            T dual_prox_inf_norm = m_preconditioner.unscale_dual_eq(m_result.lambda - m_result.y).template lpNorm<Eigen::Infinity>();
            dual_prox_inf_norm = std::max(dual_prox_inf_norm, m_preconditioner.unscale_dual_ineq(m_result.nu - m_result.z).template lpNorm<Eigen::Infinity>());
            dual_prox_inf_norm = std::max(dual_prox_inf_norm, m_preconditioner.unscale_dual_lb(nu_lb - z_lb).template lpNorm<Eigen::Infinity>());
            dual_prox_inf_norm = std::max(dual_prox_inf_norm, m_preconditioner.unscale_dual_ub(nu_ub - z_ub).template lpNorm<Eigen::Infinity>());

            T dual_inf_norm = m_preconditioner.unscale_primal_res_eq(ry).template lpNorm<Eigen::Infinity>();
            dual_inf_norm = std::max(dual_inf_norm, m_preconditioner.unscale_primal_res_ineq(rz).template lpNorm<Eigen::Infinity>());
            dual_inf_norm = std::max(dual_inf_norm, m_preconditioner.unscale_primal_res_lb(rz_lb.head(m_data.n_lb)).template lpNorm<Eigen::Infinity>());
            dual_inf_norm = std::max(dual_inf_norm, m_preconditioner.unscale_primal_res_ub(rz_ub.head(m_data.n_ub)).template lpNorm<Eigen::Infinity>());

            if (m_result.info.no_dual_update > 5 && dual_prox_inf_norm > 1e10 && dual_inf_norm < m_settings.feas_tol_abs)
            {
                m_result.info.status = Status::PIQP_PRIMAL_INFEASIBLE;
                return m_result.info.status;
            }

            if (m_result.info.no_primal_update > 5 &&
                m_preconditioner.unscale_primal(m_result.x - m_result.zeta).template lpNorm<Eigen::Infinity>() > 1e10 &&
                m_preconditioner.unscale_dual_res(rx).template lpNorm<Eigen::Infinity>() < m_settings.feas_tol_abs)
            {
                m_result.info.status = Status::PIQP_DUAL_INFEASIBLE;
                return m_result.info.status;
            }

            m_result.info.iter++;

            // avoid possibility of converging to a local minimum -> decrease the minimum regularization value
            if ((m_result.info.no_primal_update > 5 && m_result.info.rho == m_result.info.reg_limit && m_result.info.reg_limit != 1e-13) ||
                (m_result.info.no_dual_update > 5 && m_result.info.delta == m_result.info.reg_limit && m_result.info.reg_limit != 1e-13))
            {
                m_result.info.reg_limit = 1e-13;
                m_result.info.no_primal_update = 0;
                m_result.info.no_dual_update = 0;
            }

            m_kkt.update_scalings(m_result.info.rho, m_result.info.delta,
                                  m_result.s, m_result.s_lb, m_result.s_ub,
                                  m_result.z, m_result.z_lb, m_result.z_ub);
            m_kkt_init_state = false;
            bool kkt_success = m_kkt.factorize();
            if (!kkt_success)
            {
                if (m_result.info.factor_retires < m_settings.max_factor_retires)
                {
                    m_result.info.delta *= 100;
                    m_result.info.rho *= 100;
                    m_result.info.iter--;
                    m_result.info.factor_retires++;
                    m_result.info.reg_limit = std::min(10 * m_result.info.reg_limit, m_settings.feas_tol_abs);
                    continue;
                }
                else
                {
                    m_result.info.status = Status::PIQP_NUMERICS;
                    return m_result.info.status;
                }
            }
            m_result.info.factor_retires = 0;

            if (m_data.m + m_data.n_lb + m_data.n_ub > 0)
            {
                // ------------------ predictor step ------------------
                rs.array() = -m_result.s.array() * m_result.z.array();
                rs_lb.head(m_data.n_lb).array() = -s_lb.array() * z_lb.array();
                rs_ub.head(m_data.n_ub).array() = -s_ub.array() * z_ub.array();

                m_kkt.solve(rx, ry, rz, rz_lb, rz_ub, rs, rs_lb, rs_ub,
                            dx, dy, dz, dz_lb, dz_ub, ds, ds_lb, ds_ub);

                // step in the non-negative orthant
                T alpha_s = T(1);
                T alpha_z = T(1);
                for (isize i = 0; i < m_data.m; i++)
                {
                    if (ds(i) < 0)
                    {
                        alpha_s = std::min(alpha_s, -m_result.s(i) / ds(i));
                    }
                    if (dz(i) < 0)
                    {
                        alpha_z = std::min(alpha_z, -m_result.z(i) / dz(i));
                    }
                }
                for (isize i = 0; i < m_data.n_lb; i++)
                {
                    if (ds_lb(i) < 0)
                    {
                        alpha_s = std::min(alpha_s, -m_result.s_lb(i) / ds_lb(i));
                    }
                    if (dz_lb(i) < 0)
                    {
                        alpha_z = std::min(alpha_z, -m_result.z_lb(i) / dz_lb(i));
                    }
                }
                for (isize i = 0; i < m_data.n_ub; i++)
                {
                    if (ds_ub(i) < 0)
                    {
                        alpha_s = std::min(alpha_s, -m_result.s_ub(i) / ds_ub(i));
                    }
                    if (dz_ub(i) < 0)
                    {
                        alpha_z = std::min(alpha_z, -m_result.z_ub(i) / dz_ub(i));
                    }
                }
                // avoid getting to close to the boundary
                alpha_s *= m_settings.tau;
                alpha_z *= m_settings.tau;

                m_result.info.sigma = (m_result.s + alpha_s * ds).dot(m_result.z + alpha_z * dz);
                m_result.info.sigma += (s_lb + alpha_s * ds_lb.head(m_data.n_lb)).dot(z_lb + alpha_z * dz_lb.head(m_data.n_lb));
                m_result.info.sigma += (s_ub + alpha_s * ds_ub.head(m_data.n_ub)).dot(z_ub + alpha_z * dz_ub.head(m_data.n_ub));
                m_result.info.sigma /= (m_result.info.mu * (m_data.m + m_data.n_lb + m_data.n_ub));
                m_result.info.sigma = m_result.info.sigma * m_result.info.sigma * m_result.info.sigma;

                // ------------------ corrector step ------------------
                rs.array() += -ds.array() * dz.array() + m_result.info.sigma * m_result.info.mu;
                rs_lb.head(m_data.n_lb).array() += -ds_lb.head(m_data.n_lb).array() * dz_lb.head(m_data.n_lb).array() + m_result.info.sigma * m_result.info.mu;
                rs_ub.head(m_data.n_ub).array() += -ds_ub.head(m_data.n_ub).array() * dz_ub.head(m_data.n_ub).array() + m_result.info.sigma * m_result.info.mu;

                m_kkt.solve(rx, ry, rz, rz_lb, rz_ub, rs, rs_lb, rs_ub,
                            dx, dy, dz, dz_lb, dz_ub, ds, ds_lb, ds_ub);

                // step in the non-negative orthant
                alpha_s = T(1);
                alpha_z = T(1);
                for (isize i = 0; i < m_data.m; i++)
                {
                    if (ds(i) < 0)
                    {
                        alpha_s = std::min(alpha_s, -m_result.s(i) / ds(i));
                    }
                    if (dz(i) < 0)
                    {
                        alpha_z = std::min(alpha_z, -m_result.z(i) / dz(i));
                    }
                }
                for (isize i = 0; i < m_data.n_lb; i++)
                {
                    if (ds_lb(i) < 0)
                    {
                        alpha_s = std::min(alpha_s, -m_result.s_lb(i) / ds_lb(i));
                    }
                    if (dz_lb(i) < 0)
                    {
                        alpha_z = std::min(alpha_z, -m_result.z_lb(i) / dz_lb(i));
                    }
                }
                for (isize i = 0; i < m_data.n_ub; i++)
                {
                    if (ds_ub(i) < 0)
                    {
                        alpha_s = std::min(alpha_s, -m_result.s_ub(i) / ds_ub(i));
                    }
                    if (dz_ub(i) < 0)
                    {
                        alpha_z = std::min(alpha_z, -m_result.z_ub(i) / dz_ub(i));
                    }
                }
                // avoid getting to close to the boundary
                m_result.info.primal_step = alpha_s * m_settings.tau;
                m_result.info.dual_step = alpha_z * m_settings.tau;

                // ------------------ update ------------------
                m_result.x += m_result.info.primal_step * dx;
                m_result.y += m_result.info.dual_step * dy;
                m_result.z += m_result.info.dual_step * dz;
                z_lb += m_result.info.dual_step * dz_lb.head(m_data.n_lb);
                z_ub += m_result.info.dual_step * dz_ub.head(m_data.n_ub);
                m_result.s += m_result.info.primal_step * ds;
                s_lb += m_result.info.primal_step * ds_lb.head(m_data.n_lb);
                s_ub += m_result.info.primal_step * ds_ub.head(m_data.n_ub);

                T mu_prev = m_result.info.mu;
                m_result.info.mu = (m_result.s.dot(m_result.z) + s_lb.dot(z_lb) + s_ub.dot(z_ub) ) / (m_data.m + m_data.n_lb + m_data.n_ub);
                T mu_rate = std::abs(mu_prev - m_result.info.mu) / mu_prev;

                // ------------------ update regularization ------------------
                update_nr_residuals();

                if (m_preconditioner.unscale_dual_res(rx_nr).template lpNorm<Eigen::Infinity>() < 0.95 * m_result.info.dual_inf)
                {
                    m_result.zeta = m_result.x;
                    m_result.info.rho = std::max(m_result.info.reg_limit, (T(1) - mu_rate) * m_result.info.rho);
                }
                else
                {
                    m_result.info.no_primal_update++;
                    m_result.info.rho = std::max(m_result.info.reg_limit, (T(1) - 0.666 * mu_rate) * m_result.info.rho);
                }

                T dual_nr_inf_norm = m_preconditioner.unscale_primal_res_eq(ry_nr).template lpNorm<Eigen::Infinity>();
                dual_nr_inf_norm = std::max(dual_nr_inf_norm, m_preconditioner.unscale_primal_res_ineq(rz_nr).template lpNorm<Eigen::Infinity>());
                dual_nr_inf_norm = std::max(dual_nr_inf_norm, m_preconditioner.unscale_primal_res_lb(rz_lb_nr.head(m_data.n_lb)).template lpNorm<Eigen::Infinity>());
                dual_nr_inf_norm = std::max(dual_nr_inf_norm, m_preconditioner.unscale_primal_res_ub(rz_ub_nr.head(m_data.n_ub)).template lpNorm<Eigen::Infinity>());
                if (dual_nr_inf_norm < 0.95 * m_result.info.primal_inf)
                {
                    m_result.lambda = m_result.y;
                    m_result.nu = m_result.z;
                    nu_lb = z_lb;
                    nu_ub = z_ub;
                    m_result.info.delta = std::max(m_result.info.reg_limit, (T(1) - mu_rate) * m_result.info.delta);
                }
                else
                {
                    m_result.info.no_dual_update++;
                    m_result.info.delta = std::max(m_result.info.reg_limit, (T(1) - 0.666 * mu_rate) * m_result.info.delta);
                }
            }
            else
            {
                // since there are no inequalities we can take full steps
                m_kkt.solve(rx, ry, rz, rz_lb, rz_ub, rs, rs_lb, rs_ub,
                            dx, dy, dz, dz_lb, dz_ub, ds, ds_lb, ds_ub);

                m_result.info.primal_step = T(1);
                m_result.info.dual_step = T(1);
                m_result.x += m_result.info.primal_step * dx;
                m_result.y += m_result.info.dual_step * dy;

                // ------------------ update regularization ------------------
                update_nr_residuals();

                if (m_preconditioner.unscale_dual_res(rx_nr).template lpNorm<Eigen::Infinity>() < 0.95 * m_result.info.dual_inf)
                {
                    m_result.zeta = m_result.x;
                    m_result.info.rho = std::max(m_result.info.reg_limit, 0.1 * m_result.info.rho);
                }
                else
                {
                    m_result.info.no_primal_update++;
                    m_result.info.rho = std::max(m_result.info.reg_limit, 0.5 * m_result.info.rho);
                }

                if (m_preconditioner.unscale_primal_res_eq(ry_nr).template lpNorm<Eigen::Infinity>() < 0.95 * m_result.info.primal_inf)
                {
                    m_result.lambda = m_result.y;
                    m_result.info.delta = std::max(m_result.info.reg_limit, 0.1 * m_result.info.delta);
                }
                else
                {
                    m_result.info.no_dual_update++;
                    m_result.info.delta = std::max(m_result.info.reg_limit, 0.5 * m_result.info.delta);
                }
            }
        }

        m_result.info.status = Status::PIQP_MAX_ITER_REACHED;
        return m_result.info.status;
    }

    void update_nr_residuals()
    {
        rx_nr.noalias() = -m_data.P_utri * m_result.x;
        rx_nr.noalias() -= m_data.P_utri.transpose().template triangularView<Eigen::StrictlyLower>() * m_result.x;
        dual_rel_inf = m_preconditioner.unscale_dual_res(rx_nr).template lpNorm<Eigen::Infinity>();
        rx_nr.noalias() -= m_data.c;
        dx.noalias() = m_data.AT * m_result.y; // use dx as a temporary
        dual_rel_inf = std::max(dual_rel_inf, m_preconditioner.unscale_dual_res(dx).template lpNorm<Eigen::Infinity>());
        rx_nr.noalias() -= dx;
        dx.noalias() = m_data.GT * m_result.z; // use dx as a temporary
        dual_rel_inf = std::max(dual_rel_inf, m_preconditioner.unscale_dual_res(dx).template lpNorm<Eigen::Infinity>());
        rx_nr.noalias() -= dx;
        dx.setZero(); // use dx as a temporary
        for (isize i = 0; i < m_data.n_lb; i++)
        {
            dx(m_data.x_lb_idx(i)) = -m_result.z_lb(i);
        }
        dual_rel_inf = std::max(dual_rel_inf, m_preconditioner.unscale_dual_res(dx).template lpNorm<Eigen::Infinity>());
        rx_nr.noalias() -= dx;
        dx.setZero(); // use dx as a temporary
        for (isize i = 0; i < m_data.n_ub; i++)
        {
            dx(m_data.x_ub_idx(i)) = m_result.z_ub(i);
        }
        dual_rel_inf = std::max(dual_rel_inf, m_preconditioner.unscale_dual_res(dx).template lpNorm<Eigen::Infinity>());
        rx_nr.noalias() -= dx;

        ry_nr.noalias() = -m_data.AT.transpose() * m_result.x;
        primal_rel_inf = m_preconditioner.unscale_primal_res_eq(ry_nr).template lpNorm<Eigen::Infinity>();
        ry_nr.noalias() += m_data.b;
        primal_rel_inf = std::max(primal_rel_inf, m_preconditioner.unscale_primal_res_eq(m_data.b).template lpNorm<Eigen::Infinity>());

        rz_nr.noalias() = -m_data.GT.transpose() * m_result.x;
        primal_rel_inf = std::max(primal_rel_inf, m_preconditioner.unscale_primal_res_ineq(rz_nr).template lpNorm<Eigen::Infinity>());
        rz_nr.noalias() += m_data.h - m_result.s;
        primal_rel_inf = std::max(primal_rel_inf, m_preconditioner.unscale_primal_res_ineq(m_data.h).template lpNorm<Eigen::Infinity>());

        for (isize i = 0; i < m_data.n_lb; i++)
        {
            rz_lb_nr(i) = m_result.x(m_data.x_lb_idx(i)) + m_data.x_lb_n(i) - m_result.s_lb(i);

        }
        primal_rel_inf = std::max(primal_rel_inf, m_preconditioner.unscale_primal_res_lb(rz_lb_nr.head(m_data.n_lb)).template lpNorm<Eigen::Infinity>());
        primal_rel_inf = std::max(primal_rel_inf, m_preconditioner.unscale_primal_res_lb(m_data.x_lb_n.head(m_data.n_lb)).template lpNorm<Eigen::Infinity>());
        for (isize i = 0; i < m_data.n_ub; i++)
        {
            rz_ub_nr(i) = -m_result.x(m_data.x_ub_idx(i)) + m_data.x_ub(i) - m_result.s_ub(i);

        }
        primal_rel_inf = std::max(primal_rel_inf, m_preconditioner.unscale_primal_res_ub(rz_ub_nr.head(m_data.n_ub)).template lpNorm<Eigen::Infinity>());
        primal_rel_inf = std::max(primal_rel_inf, m_preconditioner.unscale_primal_res_ub(m_data.x_ub.head(m_data.n_ub)).template lpNorm<Eigen::Infinity>());
    }

    void restore_box_dual()
    {
        m_result.z_lb.tail(m_data.n - m_data.n_lb).setZero();
        m_result.z_ub.tail(m_data.n - m_data.n_ub).setZero();
        m_result.s_lb.tail(m_data.n - m_data.n_lb).array() = std::numeric_limits<T>::infinity();
        m_result.s_ub.tail(m_data.n - m_data.n_ub).array() = std::numeric_limits<T>::infinity();
        m_result.nu_lb.tail(m_data.n - m_data.n_lb).setZero();
        m_result.nu_ub.tail(m_data.n - m_data.n_ub).setZero();
        for (isize i = m_data.n_lb - 1; i >= 0; i--)
        {
            std::swap(m_result.z_lb(i), m_result.z_lb(m_data.x_lb_idx(i)));
            std::swap(m_result.s_lb(i), m_result.s_lb(m_data.x_lb_idx(i)));
            std::swap(m_result.nu_lb(i), m_result.nu_lb(m_data.x_lb_idx(i)));
        }
        for (isize i = m_data.n_ub - 1; i >= 0; i--)
        {
            std::swap(m_result.z_ub(i), m_result.z_ub(m_data.x_ub_idx(i)));
            std::swap(m_result.s_ub(i), m_result.s_ub(m_data.x_ub_idx(i)));
            std::swap(m_result.nu_ub(i), m_result.nu_ub(m_data.x_ub_idx(i)));
        }
    }

    void unscale_results()
    {
        m_result.x = m_preconditioner.unscale_primal(m_result.x);
        m_result.y = m_preconditioner.unscale_dual_eq(m_result.y);
        m_result.z = m_preconditioner.unscale_dual_ineq(m_result.z);
        m_result.z_lb.head(m_data.n_lb) = m_preconditioner.unscale_dual_lb(m_result.z_lb.head(m_data.n_lb));
        m_result.z_ub.head(m_data.n_ub) = m_preconditioner.unscale_dual_ub(m_result.z_ub.head(m_data.n_ub));
        m_result.s = m_preconditioner.unscale_slack_ineq(m_result.s);
        m_result.s_lb.head(m_data.n_lb) = m_preconditioner.unscale_slack_lb(m_result.s_lb.head(m_data.n_lb));
        m_result.s_ub.head(m_data.n_ub) = m_preconditioner.unscale_slack_ub(m_result.s_ub.head(m_data.n_ub));
        m_result.zeta = m_preconditioner.unscale_primal(m_result.zeta);
        m_result.lambda = m_preconditioner.unscale_dual_eq(m_result.lambda);
        m_result.nu = m_preconditioner.unscale_dual_ineq(m_result.nu);
        m_result.nu_lb.head(m_data.n_lb) = m_preconditioner.unscale_dual_lb(m_result.nu_lb.head(m_data.n_lb));
        m_result.nu_ub.head(m_data.n_ub) = m_preconditioner.unscale_dual_ub(m_result.nu_ub.head(m_data.n_ub));
    }
};

template<typename T, typename Preconditioner = dense::RuizEquilibration<T>>
class DenseSolver : public SolverBase<DenseSolver<T, Preconditioner>, T, int, Preconditioner, PIQP_DENSE, KKTMode::KKT_FULL>
{
public:
    void setup(const CMatRef<T>& P,
               const CVecRef<T>& c,
               const CMatRef<T>& A,
               const CVecRef<T>& b,
               const CMatRef<T>& G,
               const CVecRef<T>& h,
               const optional<CVecRef<T>> x_lb,
               const optional<CVecRef<T>> x_ub)
    {
        this->setup_impl(P, c, A, b, G, h, x_lb, x_ub);
    }

    void update(const optional<CMatRef<T>> P,
                const optional<CVecRef<T>> c,
                const optional<CMatRef<T>> A,
                const optional<CVecRef<T>> b,
                const optional<CMatRef<T>> G,
                const optional<CVecRef<T>> h,
                const optional<CVecRef<T>> x_lb,
                const optional<CVecRef<T>> x_ub,
                bool reuse_preconditioner = true)
    {
        if (!this->m_setup_done)
        {
            eigen_assert(false && "Solver not setup yet");
            return;
        }

        if (this->m_settings.compute_timings)
        {
            this->m_timer.start();
        }

        this->m_preconditioner.unscale_data(this->m_data);

        int update_options = KKTUpdateOptions::KKT_UPDATE_NONE;

        if (P.has_value())
        {
            eigen_assert(P->rows() == this->m_data.n && P->cols() == this->m_data.n && "P has wrong dimensions");
            this->m_data.P_utri = P->template triangularView<Eigen::Upper>();

            update_options |= KKTUpdateOptions::KKT_UPDATE_P;
        }

        if (A.has_value())
        {
            eigen_assert(A->rows() == this->m_data.p && A->cols() == this->m_data.n && "A has wrong dimensions");
            this->m_data.AT = A->transpose();

            update_options |= KKTUpdateOptions::KKT_UPDATE_A;
        }

        if (G.has_value())
        {
            eigen_assert(G->rows() == this->m_data.m && G->cols() == this->m_data.n && "G has wrong dimensions");
            this->m_data.GT = G->transpose();

            update_options |= KKTUpdateOptions::KKT_UPDATE_G;
        }

        if (c.has_value())
        {
            eigen_assert(c->size() == this->m_data.n && "c has wrong dimensions");
            this->m_data.c = *c;
        }

        if (b.has_value())
        {
            eigen_assert(b->size() == this->m_data.p && "b has wrong dimensions");
            this->m_data.b = *b;
        }

        if (h.has_value())
        {
            eigen_assert(h->size() == this->m_data.m && "h has wrong dimensions");
            this->m_data.h = *h;
        }

        if (x_lb.has_value()) { eigen_assert(x_lb->size() == this->m_data.n && "x_lb has wrong dimensions"); }
        if (x_ub.has_value()) { eigen_assert(x_ub->size() == this->m_data.n && "x_ub has wrong dimensions"); }
        if (x_lb.has_value()) { this->setup_lb_data(x_lb); }
        if (x_ub.has_value()) { this->setup_ub_data(x_ub); }

        this->m_preconditioner.scale_data(this->m_data, reuse_preconditioner, this->m_settings.preconditioner_iter);

        this->m_kkt.update_data(update_options);

        if (this->m_settings.compute_timings)
        {
            T update_time = this->m_timer.stop();
            this->m_result.info.update_time = update_time;
            this->m_result.info.run_time += update_time;
        }
    }
};

template<typename T, typename I, int Mode = KKTMode::KKT_FULL, typename Preconditioner = sparse::RuizEquilibration<T, I>>
class SparseSolver : public SolverBase<SparseSolver<T, I, Mode, Preconditioner>, T, I, Preconditioner, PIQP_SPARSE, Mode>
{
public:
    void setup(const SparseMat<T, I>& P,
               const CVecRef<T>& c,
               const SparseMat<T, I>& A,
               const CVecRef<T>& b,
               const SparseMat<T, I>& G,
               const CVecRef<T>& h,
               const optional<CVecRef<T>> x_lb,
               const optional<CVecRef<T>> x_ub)
    {
        this->setup_impl(P, c, A, b, G, h, x_lb, x_ub);
    }

    void update(const optional<SparseMat<T, I>> P,
                const optional<CVecRef<T>> c,
                const optional<SparseMat<T, I>> A,
                const optional<CVecRef<T>> b,
                const optional<SparseMat<T, I>> G,
                const optional<CVecRef<T>> h,
                const optional<CVecRef<T>> x_lb,
                const optional<CVecRef<T>> x_ub,
                bool reuse_preconditioner = true)
    {
        if (!this->m_setup_done)
        {
            eigen_assert(false && "Solver not setup yet");
            return;
        }

        if (this->m_settings.compute_timings)
        {
            this->m_timer.start();
        }

        this->m_preconditioner.unscale_data(this->m_data);

        int update_options = KKTUpdateOptions::KKT_UPDATE_NONE;

        if (P.has_value())
        {
            const SparseMat<T, I>& P_ = *P;

            eigen_assert(P_.rows() == this->m_data.n && P_.cols() == this->m_data.n && "P has wrong dimensions");
            isize n = P_.outerSize();
            for (isize j = 0; j < n; j++)
            {
                PIQP_MAYBE_UNUSED isize P_col_nnz = P_.outerIndexPtr()[j + 1] - P_.outerIndexPtr()[j];
                isize P_utri_col_nnz = this->m_data.P_utri.outerIndexPtr()[j + 1] - this->m_data.P_utri.outerIndexPtr()[j];
                eigen_assert(P_col_nnz >= P_utri_col_nnz && "P nonzeros missmatch");
                Eigen::Map<Vec<T>>(this->m_data.P_utri.valuePtr() + this->m_data.P_utri.outerIndexPtr()[j], P_utri_col_nnz) = Eigen::Map<const Vec<T>>(P_.valuePtr() + P_.outerIndexPtr()[j], P_utri_col_nnz);
            }

            update_options |= KKTUpdateOptions::KKT_UPDATE_P;
        }

        if (A.has_value())
        {
            const SparseMat<T, I>& A_ = *A;

            eigen_assert(A_.rows() == this->m_data.p && A_.cols() == this->m_data.n && "A has wrong dimensions");
            eigen_assert(A_.nonZeros() == this->m_data.AT.nonZeros() && "A nonzeros missmatch");
            sparse::transpose_no_allocation(A_, this->m_data.AT);

            update_options |= KKTUpdateOptions::KKT_UPDATE_A;
        }

        if (G.has_value())
        {
            const SparseMat<T, I>& G_ = *G;

            eigen_assert(G_.rows() == this->m_data.m && G_.cols() == this->m_data.n && "G has wrong dimensions");
            eigen_assert(G_.nonZeros() == this->m_data.GT.nonZeros() && "G nonzeros missmatch");
            sparse::transpose_no_allocation(G_, this->m_data.GT);

            update_options |= KKTUpdateOptions::KKT_UPDATE_G;
        }

        if (c.has_value())
        {
            eigen_assert(c->size() == this->m_data.n && "c has wrong dimensions");
            this->m_data.c = *c;
        }

        if (b.has_value())
        {
            eigen_assert(b->size() == this->m_data.p && "b has wrong dimensions");
            this->m_data.b = *b;
        }

        if (h.has_value())
        {
            eigen_assert(h->size() == this->m_data.m && "h has wrong dimensions");
            this->m_data.h = *h;
        }

        if (x_lb.has_value()) { eigen_assert(x_lb->size() == this->m_data.n && "x_lb has wrong dimensions"); }
        if (x_ub.has_value()) { eigen_assert(x_ub->size() == this->m_data.n && "x_ub has wrong dimensions"); }
        if (x_lb.has_value()) { this->setup_lb_data(x_lb); }
        if (x_ub.has_value()) { this->setup_ub_data(x_ub); }

        this->m_preconditioner.scale_data(this->m_data, reuse_preconditioner, this->m_settings.preconditioner_iter);

        this->m_kkt.update_data(update_options);

        if (this->m_settings.compute_timings)
        {
            T update_time = this->m_timer.stop();
            this->m_result.info.update_time = update_time;
            this->m_result.info.run_time += update_time;
        }
    }
};

} // namespace piqp

#endif //PIQP_SOLVER_HPP
