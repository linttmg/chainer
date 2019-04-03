#include "chainerx/routines/normalization.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include <nonstd/optional.hpp>

#include "chainerx/array.h"
#include "chainerx/axes.h"
#include "chainerx/backprop_mode.h"
#include "chainerx/backward_builder.h"
#include "chainerx/backward_context.h"
#include "chainerx/device.h"
#include "chainerx/dtype.h"
#include "chainerx/error.h"
#include "chainerx/graph.h"
#include "chainerx/macro.h"
#include "chainerx/routines/creation.h"
#include "chainerx/routines/math.h"
#include "chainerx/routines/routines_util.h"
#include "chainerx/routines/statistics.h"
#include "chainerx/routines/type_util.h"
#include "chainerx/scalar.h"
#include "chainerx/shape.h"

namespace chainerx {
namespace {

struct PreprocessBatchNormResult {
    // Arrays are reshaped if necessary
    Array gamma;
    Array beta;
    Array mean;
    Array var;
    Axes sorted_axis;
};

// Reshapes the array. If the shape is unchanged, an array with identical array body is returned. Note that chainerx::Reshape() returns
// a view with different array body if the shape is unchanged.
Array ReshapeOrIdentity(const Array& a, const Shape& shape) {
    if (a.shape() == shape) {
        return a;
    }
    return a.Reshape(shape);
}

void CheckBatchNormSupportedKind(const Array& array) {
    // BatchNorm only supports inputs of float kind.
    if (GetKind(array.dtype()) != DtypeKind::kFloat) {
        throw DtypeError{"BatchNorm only supports floating kind inputs."};
    }
}

// Reshapes the input arrays (except x) as needed.
// Sorted axes is also returned.
PreprocessBatchNormResult PreprocessBatchNorm(
        const Array& x, const Array& gamma, const Array& beta, const Array& mean, const Array& var, const OptionalAxes& axis) {
    CheckBatchNormSupportedKind(x);
    CheckBatchNormSupportedKind(gamma);
    CheckBatchNormSupportedKind(beta);
    CheckBatchNormSupportedKind(mean);
    CheckBatchNormSupportedKind(var);

    Axes sorted_axis = axis.has_value() ? internal::GetSortedAxes(*axis, x.ndim()) : Axes{0};

    Shape reduced_shape = internal::ReduceShape(x.shape(), sorted_axis, true);
    int64_t reduced_size = reduced_shape.GetTotalSize();

    if (gamma.GetTotalSize() != reduced_size) {
        throw DimensionError{
                "Gamma must have the same size as the reduced input. Actual: ", gamma.GetTotalSize(), ". Expected: ", reduced_size, "."};
    }
    if (beta.GetTotalSize() != reduced_size) {
        throw DimensionError{
                "Beta must have the same size as the reduced input. Actual: ", beta.GetTotalSize(), ". Expected: ", reduced_size, "."};
    }
    if (mean.GetTotalSize() != reduced_size) {
        throw DimensionError{
                "Mean must have the same size as the reduced input. Actual: ", mean.GetTotalSize(), ". Expected: ", reduced_size, "."};
    }
    if (var.GetTotalSize() != reduced_size) {
        throw DimensionError{
                "Variance must have the same size as the reduced input. Actual: ", var.GetTotalSize(), ". Expected: ", reduced_size, "."};
    }

    Array gamma_reshaped = ReshapeOrIdentity(gamma, reduced_shape);
    Array beta_reshaped = ReshapeOrIdentity(beta, reduced_shape);
    Array mean_reshaped = ReshapeOrIdentity(mean, reduced_shape);
    Array var_reshaped = ReshapeOrIdentity(var, reduced_shape);
    CHAINERX_ASSERT(gamma_reshaped.data() == gamma.data());  // No data copy should occur
    CHAINERX_ASSERT(beta_reshaped.data() == beta.data());
    CHAINERX_ASSERT(mean_reshaped.data() == mean.data());
    CHAINERX_ASSERT(var_reshaped.data() == var.data());

    return {std::move(gamma_reshaped), std::move(beta_reshaped), std::move(mean_reshaped), std::move(var_reshaped), sorted_axis};
}

Array ArrayOrZeros(const nonstd::optional<Array>& array, const Array& zeros_template, Dtype dtype) {
    if (array.has_value()) {
        if (array->dtype() == dtype) {
            return *array;
        }
        return array->AsType(dtype);
    }
    return Zeros(zeros_template.shape(), dtype, zeros_template.device());
}

Array ApplyBatchNorm(
        const Array& x,
        const Array& gamma,
        const Array& beta,
        const Array& mean,
        const Array& var,
        Scalar eps,
        const Axes& axis,
        const Array& out,
        Dtype interm_dtype) {
    if (CHAINERX_DEBUG) {
        Shape reduced_shape = internal::ReduceShape(x.shape(), axis, true);
        CHAINERX_ASSERT(gamma.shape() == reduced_shape);
        CHAINERX_ASSERT(beta.shape() == reduced_shape);

        int64_t reduced_total_size = reduced_shape.GetTotalSize();
        CHAINERX_ASSERT(mean.GetTotalSize() == reduced_total_size);
        CHAINERX_ASSERT(var.GetTotalSize() == reduced_total_size);
    }

    // TODO(hvy): Avoid AsType by passing dtype arguments to the following routines to minimize copies.
    const Array& x_cast = x.AsType(interm_dtype, false);
    const Array& gamma_cast = gamma.AsType(interm_dtype, false);
    const Array& beta_cast = beta.AsType(interm_dtype, false);
    const Array& mean_cast = mean.AsType(interm_dtype, false);
    const Array& var_cast = var.AsType(interm_dtype, false);

    Array inv_std = Reciprocal(Sqrt(var_cast + eps));

    Array out_cast = (x_cast - mean_cast) * inv_std * gamma_cast + beta_cast;

    out_cast.device().AsType(out_cast, out);

    return inv_std;
}

struct GenericBatchNormState {
    GenericBatchNormState(Array x_mean, Array x_inv_std) : x_mean{std::move(x_mean)}, x_inv_std{std::move(x_inv_std)} {}

    Array x_mean;
    Array x_inv_std;
};

}  // namespace

void GenericBatchNormForwardOp::Call(
        const Array& x,
        const Array& gamma,
        const Array& beta,
        const Array& running_mean,
        const Array& running_var,
        Scalar eps,
        Scalar decay,
        const Axes& axis,
        const Array& out,
        nonstd::optional<std::shared_ptr<void>>& state) {
    CHAINERX_ASSERT(internal::GetArrayBody(x)->nodes().empty());
    CHAINERX_ASSERT(internal::GetArrayBody(gamma)->nodes().empty());
    CHAINERX_ASSERT(internal::GetArrayBody(beta)->nodes().empty());

    CHAINERX_ASSERT(GetKind(x.dtype()) == DtypeKind::kFloat);
    CHAINERX_ASSERT(GetKind(gamma.dtype()) == DtypeKind::kFloat);
    CHAINERX_ASSERT(GetKind(beta.dtype()) == DtypeKind::kFloat);
    CHAINERX_ASSERT(GetKind(running_mean.dtype()) == DtypeKind::kFloat);
    CHAINERX_ASSERT(GetKind(running_var.dtype()) == DtypeKind::kFloat);

    // Compute the mean and variance of x with promoted dtype if the parameters have higher precisions.
    Dtype interm_dtype = ResultType(x, gamma, beta);
    const Array& x_cast = x.dtype() == interm_dtype ? x : x.AsType(interm_dtype);
    Array x_mean = Mean(x_cast, axis, true);
    Array x_var = Var(x_cast, axis, true);

    Array x_inv_std = ApplyBatchNorm(x, gamma, beta, x_mean, x_var, eps, axis, out, interm_dtype);

    Scalar inv_decay = Scalar{1.0 - static_cast<double>(decay)};
    int64_t n = x.GetTotalSize() / gamma.GetTotalSize();

    // TODO(hvy): Avoid AsType when IAdd supports mixed dtypes.
    running_mean *= decay;
    running_mean += (inv_decay * x_mean).AsType(running_mean.dtype(), false);
    running_var *= decay;
    running_var += (inv_decay * (static_cast<double>(n) / std::max(n - 1, int64_t{1})) * x_var).AsType(running_var.dtype(), false);

    if (state.has_value()) {
        state->reset(new GenericBatchNormState{std::move(x_mean), std::move(x_inv_std)});
    }
}

void GenericBatchNormBackwardOp::Call(
        const Array& x,
        const Array& gamma,
        const Array& gout,
        Scalar /*eps*/,
        const Axes& axis,
        const Array& gx,
        const Array& ggamma,
        const Array& gbeta,
        nonstd::optional<std::shared_ptr<void>>& state) {
    CHAINERX_ASSERT(internal::GetArrayBody(gout)->nodes().empty());

    // TODO(hvy): Implement recomputation of x_mean and x_inv_std in case they are not given by the state.
    CHAINERX_ASSERT(state.has_value());
    auto state_ptr = reinterpret_cast<GenericBatchNormState*>(state->get());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    // x_mean and x_inv_std must have promoted dtypes.
    const Array& x_mean = state_ptr->x_mean;
    const Array& x_inv_std = state_ptr->x_inv_std;  // Note: x_inv_std_ has the information of eps.

    Dtype interm_dtype = x_mean.dtype();

    int64_t n = x.GetTotalSize() / gamma.GetTotalSize();
    double inv_n = 1.0 / n;
    // TODO(hvy): Avoid AsType.
    Array gout_cast = gout.AsType(interm_dtype, false);
    Array x_hat = (x.AsType(interm_dtype, false) - x_mean) * x_inv_std;
    Array ggamma_cast = (gout_cast * x_hat).Sum(axis, true);
    Array gbeta_cast = gout_cast.Sum(axis, true);
    Array gx_cast = (gamma.AsType(interm_dtype, false) * x_inv_std) * (gout_cast - (x_hat * ggamma_cast + gbeta_cast) * inv_n);

    // TODO(hvy): Consider writing directly in the routines/ops above.
    Device& device = x.device();
    device.AsType(gx_cast, gx);
    device.AsType(ggamma_cast, ggamma);
    device.AsType(gbeta_cast, gbeta);
}

void GenericFixedBatchNormForwardOp::Call(
        const Array& x,
        const Array& gamma,
        const Array& beta,
        const Array& mean,
        const Array& var,
        Scalar eps,
        const Axes& axis,
        const Array& out) {
    Dtype interm_dtype = ResultType(x, gamma, beta, mean, var);
    ApplyBatchNorm(x, gamma, beta, mean, var, eps, axis, out, interm_dtype);
}

Array BatchNorm(
        const Array& x,
        const Array& gamma,
        const Array& beta,
        const Array& running_mean,
        const Array& running_var,
        Scalar eps,
        Scalar decay,
        const OptionalAxes& axis) {
    // Preprocess inputs.
    PreprocessBatchNormResult result = PreprocessBatchNorm(x, gamma, beta, running_mean, running_var, axis);
    const Array& gamma_reshaped = result.gamma;
    const Array& beta_reshaped = result.beta;
    const Array& mean_reshaped = result.mean;
    const Array& var_reshaped = result.var;
    const Axes& sorted_axis = result.sorted_axis;

    // Compute forward.
    Device& device = x.device();
    Backend& backend = device.backend();
    nonstd::optional<std::shared_ptr<void>> state{nullptr};

    Array out = EmptyLike(x, x.device());

    backend.CallOp<BatchNormForwardOp>(
            x.AsGradStopped(),
            gamma_reshaped.AsGradStopped(),
            beta_reshaped.AsGradStopped(),
            mean_reshaped,
            var_reshaped,
            eps,
            decay,
            sorted_axis,
            out,
            state);

    internal::MakeViewForForwardBackwardOutput(out);

    BackwardBuilder bb{"batch_norm", {x, gamma_reshaped, beta_reshaped}, {out}};
    if (BackwardBuilder::Target bt = bb.CreateTarget({0, 1, 2})) {
        bt.Define([state = std::move(state),
                   x_tok = bb.RetainInput(0),
                   gamma_tok = bb.RetainInput(1),
                   eps,
                   sorted_axis,
                   beta_shape = beta_reshaped.shape(),
                   beta_dtype = beta_reshaped.dtype()](BackwardContext& bctx) mutable {
            const Array& gout = *bctx.output_grad();
            const Array& x = bctx.GetRetainedInput(x_tok);
            const Array& gamma_reshaped = bctx.GetRetainedInput(gamma_tok);

            Device& device = x.device();

            Array gx = EmptyLike(x, device);
            Array ggamma = EmptyLike(gamma_reshaped, device);
            Array gbeta = Empty(beta_shape, beta_dtype, device);
            {
                // Compute backward.
                Device& device = gout.device();
                Backend& backend = device.backend();

                backend.CallOp<BatchNormBackwardOp>(x, gamma_reshaped, gout, eps, sorted_axis, gx, ggamma, gbeta, state);
                internal::MakeViewForForwardBackwardOutput(gx);
                internal::MakeViewForForwardBackwardOutput(ggamma);
                internal::MakeViewForForwardBackwardOutput(gbeta);
            }

            CHAINERX_ASSERT(internal::GetArrayBody(gx)->nodes().empty());
            CHAINERX_ASSERT(internal::GetArrayBody(ggamma)->nodes().empty());
            CHAINERX_ASSERT(internal::GetArrayBody(gbeta)->nodes().empty());

            if (bctx.next_required()) {
                BackwardBuilder bb2{"batch_norm_backward", {x, gamma_reshaped, gout}, {gx, ggamma, gbeta}};
                if (BackwardBuilder::Target bt2 = bb2.CreateTarget({0, 1, 2})) {
                    bt2.Define([x_tok = bb2.RetainInput(0),
                                gamma2_tok = bb2.RetainInput(1),
                                gout_tok = bb2.RetainInput(2),
                                eps,
                                sorted_axis,
                                gx_tok = bb2.RetainOutput(0),
                                ggamma_tok = bb2.RetainOutput(1)](BackwardContext& bctx2) {
                        const Array& x_retained = bctx2.GetRetainedInput(x_tok);
                        const Array& gamma_reshaped_retained = bctx2.GetRetainedInput(gamma2_tok);
                        const Array& gout_retained = bctx2.GetRetainedInput(gout_tok);

                        // TODO(hvy): Avoid AsType by passing dtype arguments to Mean, Var, etc. to minimize copies.
                        Dtype interm_dtype = ResultType(gout_retained, x_retained, gamma_reshaped_retained);
                        const Array& x = x_retained.AsType(interm_dtype, false);
                        const Array& gamma_reshaped = gamma_reshaped_retained.AsType(interm_dtype, false);
                        const Array& gout = gout_retained.AsType(interm_dtype, false);

                        Array ggx = ArrayOrZeros(bctx2.output_grad(0), x, interm_dtype);
                        Array gggamma = ArrayOrZeros(bctx2.output_grad(1), gamma_reshaped, interm_dtype);
                        Array ggbeta = ArrayOrZeros(bctx2.output_grad(2), gamma_reshaped, interm_dtype);

                        const Array& x_mean = Mean(x, sorted_axis, true).AsType(interm_dtype, false);
                        const Array& x_var = Var(x, sorted_axis, true).AsType(interm_dtype, false);
                        const Array& x_inv_std = Reciprocal(Sqrt(x_var + eps)).AsType(interm_dtype, false);

                        const Array& gx = bctx2.GetRetainedOutput(gx_tok).AsType(interm_dtype, false);
                        const Array& ggamma = bctx2.GetRetainedOutput(ggamma_tok).AsType(interm_dtype, false);

                        // Auxiliary values
                        int64_t n = x.GetTotalSize() / gamma_reshaped.GetTotalSize();
                        double inv_n = 1.0 / n;
                        Array r = (gx * ggx).Sum(sorted_axis, true);
                        Array coeff = gamma_reshaped * x_inv_std;
                        Array coeff_m = coeff * inv_n;
                        Array x_hat = (x - x_mean) * x_inv_std;

                        Array gggamma2 = gggamma - coeff_m * (x_hat * ggx).Sum(sorted_axis, true);
                        Array ggbeta2 = ggbeta - coeff_m * ggx.Sum(sorted_axis, true);

                        Array gx_hat2 = gggamma2 * gout - coeff_m * ggamma * ggx;
                        Array gstd2 = -x_inv_std * (r + (x_hat * gx_hat2).Sum(sorted_axis, true));
                        Array gmean2 = -x_inv_std * gx_hat2.Sum(sorted_axis, true);
                        Array gx2 = x_inv_std * gx_hat2 + inv_n * (gmean2 + x_hat * gstd2);
                        Array ggout2 = gggamma2 * x_hat + ggbeta2 + coeff * ggx;

                        Array ggamma2 = r / gamma_reshaped;

                        if (gx2.dtype() != x_retained.dtype()) {
                            gx2 = gx2.AsType(x_retained.dtype());
                        }
                        if (ggamma2.dtype() != gamma_reshaped_retained.dtype()) {
                            ggamma2 = ggamma2.AsType(gamma_reshaped_retained.dtype());
                        }

                        if (ggout2.dtype() != gout_retained.dtype()) {
                            ggout2 = ggout2.AsType(gout_retained.dtype());
                        }

                        bctx2.input_grad(0) = std::move(gx2);
                        bctx2.input_grad(1) = std::move(ggamma2);
                        bctx2.input_grad(2) = std::move(ggout2);
                    });
                }
                bb2.Finalize();
            }

            // TODO(niboshi): Assign at once
            bctx.input_grad(0) = std::move(gx);
            bctx.input_grad(1) = std::move(ggamma);
            bctx.input_grad(2) = std::move(gbeta);
        });
    }
    bb.Finalize();

    return out;
}

Array FixedBatchNorm(
        const Array& x, const Array& gamma, const Array& beta, const Array& mean, const Array& var, Scalar eps, const OptionalAxes& axis) {
    PreprocessBatchNormResult result =
            PreprocessBatchNorm(x, gamma.AsGradStopped(), beta.AsGradStopped(), mean.AsGradStopped(), var.AsGradStopped(), axis);

    Array out = EmptyLike(x, x.device());
    {
        NoBackpropModeScope scope{};
        x.device().backend().CallOp<FixedBatchNormForwardOp>(
                x.AsGradStopped(), result.gamma, result.beta, result.mean, result.var, eps, result.sorted_axis, out);
    }
    return out;
}

}  // namespace chainerx
