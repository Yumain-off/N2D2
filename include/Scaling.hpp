/*
    (C) Copyright 2019 CEA LIST. All Rights Reserved.
    Contributor(s): Olivier BICHLER (olivier.bichler@cea.fr)

    This software is governed by the CeCILL-C license under French law and
    abiding by the rules of distribution of free software.  You can  use,
    modify and/ or redistribute the software under the terms of the CeCILL-C
    license as circulated by CEA, CNRS and INRIA at the following URL
    "http://www.cecill.info".

    As a counterpart to the access to the source code and  rights to copy,
    modify and redistribute granted by the license, users are provided only
    with a limited warranty  and the software's author,  the holder of the
    economic rights,  and the successive licensors  have only  limited
    liability.

    The fact that you are presently reading this means that you have had
    knowledge of the CeCILL-C license and that you accept its terms.
*/

#ifndef N2D2_SCALING_H
#define N2D2_SCALING_H

#include <iosfwd>
#include <vector>

#ifdef CUDA
#include "CudaContext.hpp"
#include "Scaling_CUDA_Kernels.hpp"
#include "containers/CudaTensor.hpp"
#include "third_party/half.hpp"
#endif

#include "FloatT.hpp"
#include "ScalingMode.hpp"
#include "Scaling_Kernels.hpp"
#include "Cell/Cell.hpp"
#include "containers/Tensor.hpp"
#include "Export/DeepNetExport.hpp"
#include "utils/Utils.hpp"


namespace N2D2 {

class AbstractScaling {
public:
    /// Destructor
    virtual ~AbstractScaling() {};

protected:
    AbstractScaling() {
#ifdef CUDA
        const cudaDeviceProp& props = CudaContext::getDeviceProp();
        
        // TODO Optimize dimensions based on the size of the batch and cell
        mThreadsPerBlock = dim3(props.maxThreadsPerBlock/props.warpSize, 
                                props.warpSize);
        mBlocksPerGrid = dim3(16, props.multiProcessorCount);
#endif
    }

protected:
#ifdef CUDA
    dim3 mBlocksPerGrid;
    dim3 mThreadsPerBlock;
#endif
};

/**
 * Scale value with a floating-point multiplication: 
 * 
 * - return std::round(data * mScalingPerOutput[o]); if the cell is quantized
 * - return data * mScalingPerOutput[o]; otherwise
 */
class FloatingPointScaling: public AbstractScaling {
public:
    FloatingPointScaling(std::vector<Float_T> scalignPerOutput): 
                            mScalingPerOutput(std::move(scalignPerOutput)) 
    {
#ifdef CUDA
        mCudaScalingPerOutput.push_back(mScalingPerOutput);
#endif
    }

    const std::vector<Float_T>& getScalingPerOutput() const {
        return mScalingPerOutput;
    }

    template<typename T>
    void propagate(const Cell& cell, const Tensor<T>& input, Tensor<T>& output) const {
        floatingPointScaling_propagate(input, output,
                                       input.dimB(), input.dimZ(), 
                                       input.dimY(), input.dimX(),
                                       mScalingPerOutput, 
                                       cell.getQuantizedNbBits(), 
                                       DeepNetExport::isCellOutputUnsigned(cell));
    }

#ifdef CUDA
    template<typename T>
    void propagate(const Cell& cell, const CudaTensor<T>& input, CudaTensor<T>& output) const {
        cudaFloatingPointScaling_propagate(input.getDevicePtr(), output.getDevicePtr(),
                                           input.dimB(), input.dimZ(), 
                                           input.dimY(), input.dimX(),
                                           mCudaScalingPerOutput.getDevicePtr(), 
                                           cell.getQuantizedNbBits(), 
                                           DeepNetExport::isCellOutputUnsigned(cell),
                                           mBlocksPerGrid, mThreadsPerBlock);
    }
#endif

private:
    std::vector<Float_T> mScalingPerOutput;

#ifdef CUDA
    CudaTensor<Float_T> mCudaScalingPerOutput;
#endif
};

/**
 * Scale value with a fixed-point multiplication: 
 * 
 * const std::size_t HALF = 1 << (mNbFractionalBits - 1);
 * return (data * mScalingPerOutput[o] + HALF) >> mNbFractionalBits;
 * 
 */
class FixedPointScaling: public AbstractScaling {
public:
    FixedPointScaling(std::size_t nbFractionalBits, std::vector<std::int32_t> scaling)
           : mNbFractionalBits(nbFractionalBits), mScalingPerOutput(std::move(scaling))
    {
#ifdef CUDA
        mCudaScalingPerOutput.push_back(mScalingPerOutput);
#endif
    }

    const std::vector<std::int32_t>& getScalingPerOutput() const {
        return mScalingPerOutput;
    }

    std::size_t getFractionalBits() const {
        return mNbFractionalBits;
    }

    template<typename T>
    void propagate(const Cell& cell, const Tensor<T>& input, Tensor<T>& output) const {
        fixedPointScaling_propagate(input, output,
                                    input.dimB(), input.dimZ(), 
                                    input.dimY(), input.dimX(),
                                    mScalingPerOutput, mNbFractionalBits,
                                    cell.getQuantizedNbBits(), 
                                    DeepNetExport::isCellOutputUnsigned(cell));
    }
    
#ifdef CUDA
    template<typename T>
    void propagate(const Cell& cell, const CudaTensor<T>& input, CudaTensor<T>& output) const {
        cudaFixedPointScaling_propagate(input.getDevicePtr(), output.getDevicePtr(),
                                        input.dimB(), input.dimZ(), 
                                        input.dimY(), input.dimX(),
                                        mCudaScalingPerOutput.getDevicePtr(), mNbFractionalBits, 
                                        cell.getQuantizedNbBits(), 
                                        DeepNetExport::isCellOutputUnsigned(cell),
                                        mBlocksPerGrid, mThreadsPerBlock);
    }
#endif

private:
    std::size_t mNbFractionalBits;
    std::vector<std::int32_t> mScalingPerOutput;

#ifdef CUDA
    CudaTensor<std::int32_t> mCudaScalingPerOutput;
#endif
};


/**
 * Scale value with a single shift: 
 * 
 * const std::size_t HALF = 1 << (mScalingPerOutput[o] - 1);
 * return (data + HALF) >> mScalingPerOutput[o];
 * 
 */
class SingleShiftScaling: public AbstractScaling {
public:
    SingleShiftScaling(std::vector<unsigned char> scaling): mScalingPerOutput(std::move(scaling)) 
    {
#ifdef CUDA
        mCudaScalingPerOutput.push_back(mScalingPerOutput);
#endif
    }

    const std::vector<unsigned char>& getScalingPerOutput() const {
        return mScalingPerOutput;
    }

    template<typename T>
    void propagate(const Cell& cell, const Tensor<T>& input, Tensor<T>& output) const {
        singleShiftScaling_propagate(input, output,
                                     input.dimB(), input.dimZ(), 
                                     input.dimY(), input.dimX(),
                                     mScalingPerOutput, 
                                     cell.getQuantizedNbBits(), 
                                     DeepNetExport::isCellOutputUnsigned(cell));
    }
    
#ifdef CUDA
    template<typename T>
    void propagate(const Cell& cell, const CudaTensor<T>& input, CudaTensor<T>& output) const {
        cudaSingleShiftScaling_propagate(input.getDevicePtr(), output.getDevicePtr(),
                                         input.dimB(), input.dimZ(), 
                                         input.dimY(), input.dimX(),
                                         mCudaScalingPerOutput.getDevicePtr(), 
                                         cell.getQuantizedNbBits(), 
                                         DeepNetExport::isCellOutputUnsigned(cell),
                                         mBlocksPerGrid, mThreadsPerBlock);
    }
#endif

private:
    std::vector<unsigned char> mScalingPerOutput;

#ifdef CUDA
    CudaTensor<unsigned char> mCudaScalingPerOutput;
#endif
};

/**
 * Scale values with a double shift: 
 * 
 * const std::size_t HALF = 1 << (mScalingPerOutput[o].second - 1);
 * return (data + (data << mScalingPerOutput[o].first) + HALF) >> mScalingPerOutput[o].second;
 * 
 */
class DoubleShiftScaling: public AbstractScaling {
public:
    DoubleShiftScaling(std::vector<std::pair<unsigned char, unsigned char>> scaling)
                                : mScalingPerOutput(std::move(scaling)) 
    {
#ifdef CUDA
        mCudaScalingPerOutput.push_back(mScalingPerOutput);
#endif
    }

    const std::vector<std::pair<unsigned char, unsigned char>>& getScalingPerOutput() const {
        return mScalingPerOutput;
    }
    
    template<typename T>
    void propagate(const Cell& cell, const Tensor<T>& input, Tensor<T>& output) const {
        doubleShiftScaling_propagate(input, output,
                                     input.dimB(), input.dimZ(), 
                                     input.dimY(), input.dimX(),
                                     mScalingPerOutput, 
                                     cell.getQuantizedNbBits(), 
                                     DeepNetExport::isCellOutputUnsigned(cell));
    }
    
#ifdef CUDA
    template<typename T>
    void propagate(const Cell& cell, const CudaTensor<T>& input, CudaTensor<T>& output) const {
        cudaDoubleShiftScaling_propagate(input.getDevicePtr(), output.getDevicePtr(),
                                         input.dimB(), input.dimZ(), 
                                         input.dimY(), input.dimX(),
                                         mCudaScalingPerOutput.getDevicePtr(), 
                                         cell.getQuantizedNbBits(), 
                                         DeepNetExport::isCellOutputUnsigned(cell),
                                         mBlocksPerGrid, mThreadsPerBlock);
    }
#endif

private:
    std::vector<std::pair<unsigned char, unsigned char>> mScalingPerOutput;

#ifdef CUDA
    CudaTensor<std::pair<unsigned char, unsigned char>> mCudaScalingPerOutput;
#endif
};


class Scaling {
public:
    Scaling();

    static Scaling floatingPointScaling(std::vector<Float_T> scalingPerOutput) {
        return Scaling(ScalingMode::FLOAT_MULT, 
                       Utils::make_unique<FloatingPointScaling>(std::move(scalingPerOutput)));
    }

    static Scaling fixedPointScaling(std::size_t nbFractionalBits, 
                                     std::vector<std::int32_t> scalingPerOutput) 
    {
        return Scaling(ScalingMode::FIXED_MULT, 
                       Utils::make_unique<FixedPointScaling>(nbFractionalBits, 
                                                             std::move(scalingPerOutput)));
    }

    static Scaling singleShiftScaling(std::vector<unsigned char> scalingPerOutput) {
        return Scaling(ScalingMode::SINGLE_SHIFT, 
                       Utils::make_unique<SingleShiftScaling>(std::move(scalingPerOutput)));
    }

    static Scaling doubleShiftScaling(std::vector<std::pair<unsigned char, 
                                                            unsigned char>> scalingPerOutput) 
    {
        return Scaling(ScalingMode::DOUBLE_SHIFT, 
                       Utils::make_unique<DoubleShiftScaling>(std::move(scalingPerOutput)));
    }

    ScalingMode getMode() const {
        return mMode;
    }

    const FloatingPointScaling& getFloatingPointScaling() const {
        assert(mMode == ScalingMode::FLOAT_MULT);
        return static_cast<const FloatingPointScaling&>(*mScaling);
    }

    const FixedPointScaling& getFixedPointScaling() const {
        assert(mMode == ScalingMode::FIXED_MULT);
        return static_cast<const FixedPointScaling&>(*mScaling);
    }

    const SingleShiftScaling& getSingleShiftScaling() const {
        assert(mMode == ScalingMode::SINGLE_SHIFT);
        return static_cast<const SingleShiftScaling&>(*mScaling);
    }

    const DoubleShiftScaling& getDoubleShiftScaling() const {
        assert(mMode == ScalingMode::DOUBLE_SHIFT);
        return static_cast<const DoubleShiftScaling&>(*mScaling);
    }

    template<class T>
    void propagate(const Cell& cell, Tensor<T>& data) const;

    template<class T>
    void propagate(const Cell& cell, const Tensor<T>& input, Tensor<T>& output) const;

    template<class T>
    void backPropagate(const Cell& cell, Tensor<T>& data, Tensor<T>& diffData) const;

#ifdef CUDA
    template<class T>
    void propagate(const Cell& cell, CudaTensor<T>& data) const;

    template<class T>
    void propagate(const Cell& cell, const CudaTensor<T>& input, CudaTensor<T>& output) const;

    template<class T>
    void backPropagate(const Cell& cell, CudaTensor<T>& data, CudaTensor<T>& diffData) const;
#endif

private:
    Scaling(ScalingMode mode, std::unique_ptr<AbstractScaling> scaling);

private:
    ScalingMode mMode;
    std::unique_ptr<AbstractScaling> mScaling;
};

template<class T>
inline void Scaling::propagate(const Cell& cell, Tensor<T>& data) const {
    propagate(cell, data, data);
}

template<class T>
inline void Scaling::propagate(const Cell& cell, const Tensor<T>& input, Tensor<T>& output) const {
    assert(input.size() == output.size());
    
    switch(mMode) {
        case ScalingMode::NONE:
            break;
        case ScalingMode::FLOAT_MULT:
            static_cast<const FloatingPointScaling&>(*mScaling).propagate(cell, input, output);
            break;
        case ScalingMode::FIXED_MULT:
            static_cast<const FixedPointScaling&>(*mScaling).propagate(cell, input, output);
            break;
        case ScalingMode::SINGLE_SHIFT:
            static_cast<const SingleShiftScaling&>(*mScaling).propagate(cell, input, output);
            break;
        case ScalingMode::DOUBLE_SHIFT:
            static_cast<const DoubleShiftScaling&>(*mScaling).propagate(cell, input, output);
            break;
        default:
            throw std::runtime_error("Unsupported scaling propagation.");
    }
}

template<class T>
inline void Scaling::backPropagate(const Cell& /*cell*/, 
                                   Tensor<T>& /*data*/, Tensor<T>& /*diffData*/) const 
{
    if(mMode == ScalingMode::NONE) {
        return;
    }

    throw std::runtime_error("Unsupported scaling backpropagation.");
}


#ifdef CUDA
template<class T>
inline void Scaling::propagate(const Cell& cell, CudaTensor<T>& data) const {
    propagate(cell, data, data);
}

template<class T>
inline void Scaling::propagate(const Cell& cell, const CudaTensor<T>& input, CudaTensor<T>& output) const {
    switch(mMode) {
        case ScalingMode::NONE:
            break;
        case ScalingMode::FLOAT_MULT:
            static_cast<const FloatingPointScaling&>(*mScaling).propagate(cell, input, output);
            break;
        case ScalingMode::FIXED_MULT:
            static_cast<const FixedPointScaling&>(*mScaling).propagate(cell, input, output);
            break;
        case ScalingMode::SINGLE_SHIFT:
            static_cast<const SingleShiftScaling&>(*mScaling).propagate(cell, input, output);
            break;
        case ScalingMode::DOUBLE_SHIFT:
            static_cast<const DoubleShiftScaling&>(*mScaling).propagate(cell, input, output);
            break;
        default:
            throw std::runtime_error("Unsupported scaling propagation.");
    }
}

template<class T>
inline void Scaling::backPropagate(const Cell& /*cell*/, 
                                   CudaTensor<T>& /*data*/, CudaTensor<T>& /*diffData*/) const 
{
    if(mMode == ScalingMode::NONE) {
        return;
    }

    throw std::runtime_error("Unsupported scaling backpropagation.");
}
#endif

}

#endif