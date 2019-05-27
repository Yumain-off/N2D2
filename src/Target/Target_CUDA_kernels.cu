/*
    (C) Copyright 2019 CEA LIST. All Rights Reserved.
    Contributor(s): David BRIAND (david.briand@cea.fr)

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

#include "Target/Target_CUDA_kernels.hpp"

__global__
void cudaGetEstimatedTarget_kernel( const unsigned int topN,
                                    const unsigned int nbClass,
                                    const unsigned int targetHeight,
                                    const unsigned int targetWidth,
                                    const unsigned int batchSize,
                                    const unsigned int groupSize,
                                    float threshold,
                                    const float* input,
                                    float* estimatedLabelsValue,
                                    int* estimatedLabels)
{
    unsigned int outputMax = 0;
    float maxVal = 0.0f;

	const int index = threadIdx.x + blockIdx.x*blockDim.x;
	const int probabilityMapSize = targetWidth*targetHeight*nbClass;
    const int batchInputOffset = (nbClass > 1) 
                                    ? probabilityMapSize*blockIdx.z
                                    : targetWidth*targetHeight*blockIdx.z;

    const int batchOutputOffset = targetWidth*targetHeight*topN*blockIdx.z;

    if (index < targetWidth*targetHeight) {
        if(nbClass > 1)
        {
            //__syncthreads();
            if(topN > 1)
            {
                extern __shared__ float local_tmp[];
                for(unsigned int cls = 0; cls < nbClass; ++cls)
                    local_tmp[threadIdx.x*nbClass + cls]
                        = input[index + cls*targetWidth*targetHeight + batchInputOffset];

                float tmpValue = 0.0f;
                int tmpIdx = 0;

                int* idxData = (int*)&local_tmp[groupSize*nbClass]; // nF floats
                for(unsigned int cls = 0; cls < nbClass; ++cls)
                    idxData[threadIdx.x*nbClass + cls] = cls;

                //Sorting in a descending order
                for (int i = 0; i < nbClass; ++i) {
                    for (int j = 0; j < nbClass; ++j) {
                        if(local_tmp[threadIdx.x*nbClass + j] 
                                < local_tmp[threadIdx.x*nbClass + i])
                        {
                            tmpValue 
                                = local_tmp[threadIdx.x*nbClass + i];
                            local_tmp[threadIdx.x*nbClass + i] 
                                = local_tmp[threadIdx.x*nbClass + j];
                            local_tmp[threadIdx.x*nbClass + j] 
                                = tmpValue;

                            tmpIdx = idxData[threadIdx.x*nbClass + i];
                            idxData[threadIdx.x*nbClass + i] 
                                = idxData[threadIdx.x*nbClass + j];
                            idxData[threadIdx.x*nbClass + j] = tmpIdx;
                        }
                    }
                }

                //Write to output
                for (unsigned int cls = 0; cls < topN; ++cls) 
                {
                    estimatedLabelsValue[index + cls*targetWidth*targetHeight + batchOutputOffset] 
                            = local_tmp[threadIdx.x*nbClass + cls];
                    estimatedLabels[index + cls*targetWidth*targetHeight + batchOutputOffset] 
                            = idxData[threadIdx.x*nbClass + cls];

                }

            }
            else
            {
                maxVal = input[index + batchInputOffset];

                for (unsigned int cls = 1; cls < nbClass; ++cls) {
                    float tmp = input[index + cls*targetWidth*targetHeight + batchInputOffset];
                    if (tmp > maxVal) {
                        outputMax = cls;
                        maxVal = tmp;
                    }
                    
                }
                estimatedLabels[index + batchOutputOffset] = outputMax;
                estimatedLabelsValue[index + batchOutputOffset] = maxVal;

            }
        }
        else if(nbClass == 1)
        {
            const int estimatedLabel
                = (input[index + batchInputOffset] > threshold);

            estimatedLabels[index + batchOutputOffset] = estimatedLabel;
            estimatedLabelsValue[index + batchOutputOffset]
                = (estimatedLabel == 1)
                    ? input[index + batchInputOffset]
                    : (1.0 - input[index + batchInputOffset]);
        }

    }

}

__global__
void cudaGetEstimatedLabel_kernel(const float* value,
                                  unsigned int outputWidth,
                                  unsigned int outputHeight,
                                  unsigned int nbOutputs,
                                  unsigned int batchPos,
                                  unsigned int x0,
                                  unsigned int x1,
                                  unsigned int y0,
                                  unsigned int y1,
                                  float* bbLabels)
{
    const unsigned int batchOffset
        = outputWidth * outputHeight * nbOutputs * batchPos;
    const unsigned int dimZ = (nbOutputs > 1) ? nbOutputs : 2;

    for (unsigned int z = blockIdx.x; z < dimZ; z += gridDim.x) {
        for (unsigned int y = y0 + threadIdx.y; y <= y1; y += blockDim.y) {
            for (unsigned int x = x0 + threadIdx.x; x <= x1; x += blockDim.x) {
                const unsigned int idx = x + outputWidth
                    * (y + outputHeight * z * (nbOutputs > 1)) + batchOffset;

                if (nbOutputs > 1 || z > 0) {
                    atomicAdd(bbLabels + z, value[idx]);
                }
                else {
                    // nbOutputs == 1 && z == 0
                    atomicAdd(bbLabels + z, 1.0f - value[idx]);
                }
            }
        }
    }
}

static unsigned int nextDivisor(unsigned int target, unsigned int value)
{
    unsigned int v = value;
    while (target % v != 0)
        ++v;
    return v;
}

void N2D2::cudaGetEstimatedTarget(const unsigned int topN,
                                  const unsigned int nbClass,
                                  const unsigned int targetHeight,
                                  const unsigned int targetWidth,
                                  const unsigned int batchSize,
                                  float threshold,
                                  const float* input,
                                  float* estimatedLabelsValue,
                                  int* estimatedLabels)
{
    const unsigned int groupSize = min(32, targetHeight*targetWidth) ;
    const unsigned int blockSize = ceil(targetHeight*targetWidth / groupSize) ;

    const dim3 threadsPerBlocks = {groupSize, 1, 1};
    const dim3 blocksPerGrid = {blockSize , 1, batchSize};

    size_t sharedSizeF = topN > 1 ? sizeof(float) * nbClass * groupSize : 0;
    size_t sharedSizeI = topN > 1 ? sizeof(int) * nbClass * groupSize : 0;
    size_t totalSharedSize = sharedSizeI + sharedSizeF;


    cudaGetEstimatedTarget_kernel<<<blocksPerGrid, threadsPerBlocks, totalSharedSize>>>
        (topN,
           nbClass,
           targetHeight,
           targetWidth,
           batchSize,
           groupSize,
           threshold,
           input,
           estimatedLabelsValue,
           estimatedLabels);
    CHECK_CUDA_STATUS(cudaPeekAtLastError());
}

void N2D2::cudaGetEstimatedLabel(const cudaDeviceProp& deviceProp,
                                 const float* value,
                                 unsigned int outputWidth,
                                 unsigned int outputHeight,
                                 unsigned int nbOutputs,
                                 unsigned int batchPos,
                                 unsigned int x0,
                                 unsigned int x1,
                                 unsigned int y0,
                                 unsigned int y1,
                                 float* bbLabels)
{
    const unsigned int maxSize = (unsigned int)deviceProp.maxThreadsPerBlock;
    const unsigned int prefMultiple = (unsigned int)deviceProp.warpSize;

    const unsigned int dimZ = (nbOutputs > 1) ? nbOutputs : 2;
    const unsigned int sizeX = (x1 - x0 + 1);
    const unsigned int sizeY = (y1 - y0 + 1);

    const unsigned int groupSize = (sizeX * sizeY < maxSize)
                                       ? sizeX * sizeY
                                       : maxSize;
    const unsigned int groupWidth
        = min(prefMultiple, nextDivisor(groupSize, sizeX));

    const dim3 blocksPerGrid = {dimZ, 1, 1};
    const dim3 threadsPerBlocks = {groupWidth, groupSize / groupWidth, 1};

    cudaGetEstimatedLabel_kernel<<<blocksPerGrid, threadsPerBlocks>>>
        (value,
           outputWidth,
           outputHeight,
           nbOutputs,
           batchPos,
           x0,
           x1,
           y0,
           y1,
           bbLabels);
    CHECK_CUDA_STATUS(cudaPeekAtLastError());
}


