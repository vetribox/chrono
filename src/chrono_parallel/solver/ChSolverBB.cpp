#include "chrono_parallel/solver/ChSolverBB.h"
#include <blaze/math/CompressedVector.h>
using namespace chrono;

ChSolverBB::ChSolverBB() : ChSolverParallel() {}

void ChSolverBB::UpdateR() {
    const SubMatrixType& D_n_T = _DNT_;
    const DynamicVector<real>& M_invk = data_manager->host_data.M_invk;
    const DynamicVector<real>& b = data_manager->host_data.b;
    DynamicVector<real>& R = data_manager->host_data.R;
    DynamicVector<real>& s = data_manager->host_data.s;

    uint num_contacts = data_manager->num_rigid_contacts;

    s.resize(data_manager->num_rigid_contacts);
    reset(s);

    rigid_rigid->Build_s();

    ConstSubVectorType b_n = blaze::subvector(b, 0, num_contacts);
    SubVectorType R_n = blaze::subvector(R, 0, num_contacts);
    SubVectorType s_n = blaze::subvector(s, 0, num_contacts);

    R_n = -b_n - D_n_T * M_invk + s_n;
}

// Tuning of the spectral gradient search
const real a_min = 1e-13;
const real a_max = 1e13;
const real sigma_min = 0.1;
const real sigma_max = 0.9;

const real gmma = 1e-4;
const real neg_BB1_fallback = 0.11;
const real neg_BB2_fallback = 0.12;
const int n_armijo = 10;
const int max_armijo_backtrace = 3;

uint ChSolverBB::SolveBB(const uint max_iter,
                         const uint size,
                         const DynamicVector<real>& r,
                         DynamicVector<real>& gamma) {
    real& lastgoodres = data_manager->measures.solver.residual;
    lastgoodres = 10e30;
    real& objective_value = data_manager->measures.solver.objective_value;

    temp.resize(size);
    ml.resize(size);
    mg.resize(size);
    mg_p.resize(size);
    ml_candidate.resize(size);
    ms.resize(size);
    my.resize(size);
    mdir.resize(size);
    ml_p.resize(size);
    real alpha = 0.0001;
    if (data_manager->settings.solver.cache_step_length == true) {
        if (data_manager->settings.solver.solver_mode == NORMAL) {
            alpha = data_manager->measures.solver.normal_apgd_step_length;
        } else if (data_manager->settings.solver.solver_mode == SLIDING) {
            alpha = data_manager->measures.solver.sliding_apgd_step_length;
        } else if (data_manager->settings.solver.solver_mode == SPINNING) {
            alpha = data_manager->measures.solver.spinning_apgd_step_length;
        } else if (data_manager->settings.solver.solver_mode == BILATERAL) {
            alpha = data_manager->measures.solver.bilateral_apgd_step_length;
        } else {
            alpha = 0.0001;
        }
    } else {
        alpha = 0.0001;
    }
    real gdiff = 1.0 / pow(size, 2.0);
    real mf_p = 0;
    real mf = 1e29;
    ml_candidate = ml = gamma;
    ShurProduct(ml, temp);
    mg = temp - r;
    //mg_p = mg;
    f_hist.resize(0);
    f_hist.reserve(max_iter * max_armijo_backtrace);
    for (current_iteration = 0; current_iteration < max_iter; current_iteration++) {
        temp = (ml - alpha * mg);
        Project(temp.data());
        mdir = temp - ml;

        real dTg = (mdir, mg);
        real lambda = 1.0;
        for (int n_backtracks=0; n_backtracks<max_armijo_backtrace; n_backtracks++) {
            ml_p = ml + lambda * mdir;

            ShurProduct(ml_p, temp);
            mg_p = temp - r;
            mf_p = (ml_p, 0.5 * temp - r);

            f_hist.push_back(mf_p);

            real max_compare = 10e29;
            for (int h = 1; h <= Min(current_iteration, n_armijo); h++) {
                real compare = f_hist[current_iteration - h] + gmma * lambda * dTg;
                if (compare > max_compare){
                    max_compare = compare;
                }
            }
            if (mf_p > max_compare) {
                if (current_iteration > 0){
                    mf = f_hist[current_iteration - 1];
                }
                real lambdanew = -lambda * lambda * dTg / (2 * (mf_p - mf - lambda * dTg));
                lambda = Max(sigma_min * lambda, Min(sigma_max * lambda, lambdanew));
                printf("Repeat Armijo, new lambda = %f \n", lambda);
            } else {
                break;
            }
        }
        ms = ml_p - ml;
        my = mg_p - mg;
        ml = ml_p;
        mg = mg_p;
        if (current_iteration % 2 == 0) {
            real sDs = (ms, ms);
            real sy = (ms, my);
            if (sy <= 0) {
                alpha = neg_BB1_fallback;
            } else {
                alpha = Min(a_max, Max(a_min, sDs / sy));
            }
        } else {
            real sy = (ms, my);
            real yDy = (my, my);
            if (sy <= 0) {
                alpha = neg_BB2_fallback;
            } else {
                alpha = Min(a_max, Max(a_min, sy / yDy));
            }
        }
        temp = ml - gdiff * mg;
        Project(temp.data());
        temp = (ml - temp) / (-gdiff);

        real g_proj_norm = Sqrt((temp, temp));
        if (g_proj_norm < lastgoodres) {
            lastgoodres = g_proj_norm;
            ml_candidate = ml;
        }
        objective_value = mf_p;
        AtIterationEnd(lastgoodres, objective_value);
    }
    if (data_manager->settings.solver.solver_mode == NORMAL) {
        data_manager->measures.solver.normal_apgd_step_length = alpha;
    } else if (data_manager->settings.solver.solver_mode == SLIDING) {
        data_manager->measures.solver.sliding_apgd_step_length = alpha;
    } else if (data_manager->settings.solver.solver_mode == SPINNING) {
        data_manager->measures.solver.spinning_apgd_step_length = alpha;
    } else if (data_manager->settings.solver.solver_mode == BILATERAL) {
        data_manager->measures.solver.bilateral_apgd_step_length = alpha;
    }
    gamma = ml_candidate;
    data_manager->system_timer.stop("ChSolverParallel_Solve");
    return current_iteration;
}
