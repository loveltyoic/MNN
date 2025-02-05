//
//  CPUSoftmax.cpp
//  MNN
//
//  Created by MNN on 2018/07/16.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "CPUSoftmax.hpp"
#include "Concurrency.h"
#include <math.h>
#include "CPUBackend.hpp"
#include "CommonOptFunction.h"
#include "Macro.h"
#include "TensorUtils.hpp"
#ifdef MNN_USE_NEON
#include <arm_neon.h>
#endif

namespace MNN {

static void elementwiseExp(float* dst, const float* src, int dataSize) {
    int countC8        = dataSize / 8;
    if (countC8 > 0) {
        // Align to eight so asm is easier to write
        static float parameters[] = {
            (float)log(2.0f), 1.0f / (float)log(2.0f), 1.0f, 1.0f, 0.5f, 1.0f / 6.0f, 1.0f / 24.0f, 1.0f / 120.0f};
        MNNExpC8(dst, src, parameters, countC8);
    }
    int remain = countC8 * 8;
    auto param = log(2.0f);
    for (int i = remain; i < dataSize; i++) {
        /*Origin Function*/
         //dst[i] = expf(-src[i]);
        
        /*Approciate Function*/
        
        auto x         = -src[i];
        int div        = (x / param);
        auto xReamin   = x - div * param;
        div            = std::min(div, 24);
        div            = std::max(div, -24);
        float expBasic = 1.0;
        if (div < 0) {
            expBasic = 1.0f / (1 << (-div));
        } else {
            expBasic = (float)(1 << div);
        }
        auto t         = xReamin;
        auto expRemain = ((((1.0f / 120 * t + 1.0f / 24) * t + 1.0f / 6) * t + 0.5f) * t + 1.0f) * t + 1.0f;
        dst[i]  = expBasic * expRemain;
    }
}
    
static int _softmax1(const float *srcData, float *dstData, int outside, int channel, int threadNum) {
    MNN_CONCURRENCY_BEGIN(tId, threadNum);
    {
        const float *srcY = srcData + tId * channel;
        float *dstY       = dstData + tId * channel;
        for (int y = (int)tId; y < outside; y += threadNum, srcY += channel*threadNum, dstY += channel*threadNum) {
            float maxValue = srcY[0];
            {
                int c = 1;
#ifdef MNN_USE_NEON
#if !(defined(__ARM_FEATURE_FMA) && defined(__aarch64__))
#define vmaxvq_f32(v)                 \
    ({                                \
        float __m = v[0];             \
        for (int i = 1; i < 4; i++) { \
            if (v[i] > __m)           \
                __m = v[i];           \
        }                             \
        __m;                          \
    })
#endif
                if (c + 3 < channel) {
                    float32x4_t maxx4 = vld1q_f32(srcY + c);
                    c += 4;
                    for (; c + 3 < channel; c += 4) {
                        maxx4 = vmaxq_f32(maxx4, vld1q_f32(srcY + c));
                    }
                    float value = vmaxvq_f32(maxx4);
                    if (value > maxValue)
                        maxValue = value;
                }
#endif
                for (; c < channel; ++c) {
                    float value = srcY[c];
                    if (value > maxValue)
                        maxValue = value;
                }
            }
            
            for (int c = 0; c < channel; ++c) {
                dstY[c] = -srcY[c] + maxValue;
            }
            
            elementwiseExp(dstY, dstY, channel);
            
            // sum
            float sumValue = 0;

            for (int c = 0; c < channel; ++c) {
                sumValue += dstY[c];
            }
            
            // div
            {
                int c = 0;
#ifdef MNN_USE_NEON
                float div = 1.f / sumValue;
                for (; c + 3 < channel; c += 4) {
                    vst1q_f32(dstY + c, vmulq_n_f32(vld1q_f32(dstY + c), div));
                }
#endif
                for (; c < channel; ++c) {
                    dstY[c] /= sumValue;
                }
            }
        }
    }
    MNN_CONCURRENCY_END();

    return 0;
}
static int _softmaxCommon(const float *srcData, float *dstData, int inside, int outside, int channel, float* maxValue, float* sumValue, int threadNum) {
    if (inside == 1)
        return _softmax1(srcData, dstData, outside, channel, threadNum);
    
    const int stepY   = inside * channel;
    MNN_CONCURRENCY_BEGIN(tId, threadNum);
    {
        const float *srcY = srcData + tId * stepY;
        float *dstY       = dstData + tId * stepY;
        float* maxValueSub = maxValue + tId * inside;
        float* sumValueSub = sumValue + tId * inside;
        
        for (int y = (int)tId; y < outside; y += threadNum, srcY += stepY * threadNum, dstY += stepY * threadNum) {
            memcpy(maxValueSub, srcY, sizeof(float) * inside);
            const float *src = srcY + inside;
            for (int c = 1; c < channel; ++c, src += inside) {
                for (int x = 0; x < inside; ++x) {
                    if (src[x] > maxValueSub[x])
                        maxValueSub[x] = src[x];
                }
            }
            memset(sumValueSub, 0, sizeof(float) * inside);
            src        = srcY;
            float *dst = dstY;
            for (int c = 0; c < channel; ++c, src += inside, dst += inside) {
                for (int x = 0; x < inside; ++x) {
                    dst[x] = -src[x] + maxValueSub[x];
                }
            }
            
            dst = dstY;
            elementwiseExp(dst, dst, inside*channel);
            
            for (int c = 0; c < channel; ++c, src += inside, dst += inside) {
                for (int x = 0; x < inside; ++x) {
                    sumValueSub[x] += dst[x];
                }
            }
            
            dst = dstY;
            for (int c = 0; c < channel; ++c, dst += inside) {
                for (int x = 0; x < inside; ++x) {
                    dst[x] /= sumValueSub[x];
                }
            }
        }
    }
    MNN_CONCURRENCY_END();
    return 0;
}

ErrorCode CPUSoftmax::onResize(const std::vector<Tensor *> &inputs, const std::vector<Tensor *> &outputs) {
    auto input           = inputs[0];
    const int dimensions = input->buffer().dimensions;
    
    const auto layout = TensorUtils::getDescribe(input)->dimensionFormat;
    mNeedUnpackC4     = layout == MNN_DATA_FORMAT_NC4HW4;
    
    if (mNeedUnpackC4) {
        int totalSize = 1;
        for (int i = 0; i < dimensions; ++i) {
            totalSize *= input->length(i);
        }
        mStorage.buffer().dim[0].extent = 1;
        mStorage.buffer().dim[1].extent = totalSize;
        mStorage.buffer().dim[1].flags  = 0;
        mStorage.buffer().dimensions    = 2;
        mStorage.buffer().type          = input->getType();
        backend()->onAcquireBuffer(&mStorage, Backend::DYNAMIC);
    }
    
    int inside = 1;
    int dims = input->buffer().dimensions;
    for (int i = mAxis + 1; i < dims; ++i) {
        inside *= input->length(i);
    }

    if (inside != 1) { // not run _softmax1, we need maxValue Tensor and sumValue Tensor.
        int threadNum = ((CPUBackend*)backend())->threadNumber();
        
        mMaxValue.buffer().dim[0].extent = inside * threadNum;
        mMaxValue.buffer().dimensions    = 1;
        mMaxValue.setType(DataType_DT_FLOAT);
        backend()->onAcquireBuffer(&mMaxValue, Backend::DYNAMIC);
        
        mSumValue.buffer().dim[0].extent = inside * threadNum;
        mSumValue.buffer().dimensions    = 1;
        mSumValue.setType(DataType_DT_FLOAT);
        backend()->onAcquireBuffer(&mSumValue, Backend::DYNAMIC);
        
        backend()->onReleaseBuffer(&mMaxValue, Backend::DYNAMIC);
        backend()->onReleaseBuffer(&mSumValue, Backend::DYNAMIC);
    }
    
    if (mNeedUnpackC4) {
        backend()->onReleaseBuffer(&mStorage, Backend::DYNAMIC);
    }
    
    return NO_ERROR;
}

ErrorCode CPUSoftmax::onExecute(const std::vector<Tensor *> &inputs, const std::vector<Tensor *> &outputs) {
    MNN_ASSERT(1 == inputs.size());
    MNN_ASSERT(1 == outputs.size());
    auto inputTensor        = inputs[0];
    auto outputTensor       = outputs[0];
    const auto inputDataPtr = inputTensor->host<float>();
    auto outputDataPtr      = outputTensor->host<float>();
    const int batch         = inputTensor->batch();
    const auto dims         = inputTensor->buffer().dimensions;

    float *tempData = nullptr;
    if (mNeedUnpackC4) {
        tempData = mStorage.host<float>();
    }
    
    int areaInput = 1;
    for (int i = 2; i < dims; ++i) {
        areaInput *= inputTensor->length(i);
    }
    int inside  = 1;
    int outside = 1;
    int channel = 1;
    for (int i = 1; i < mAxis; ++i) {
        outside *= inputTensor->length(i);
    }
    channel = inputTensor->length(mAxis);
    for (int i = mAxis + 1; i < dims; ++i) {
        inside *= inputTensor->length(i);
    }

    int threadNum = ((CPUBackend*)backend())->threadNumber();
    int batchSize = outputTensor->size() / sizeof(float) / batch;
    for (int batchIndex = 0; batchIndex < batch; ++batchIndex) {
        auto inputData  = inputDataPtr + batchIndex * batchSize;
        auto outputData = outputDataPtr + batchIndex * batchSize;
        if (1 == areaInput || !mNeedUnpackC4) {
            _softmaxCommon(inputData, outputData, inside, outside, channel, mMaxValue.host<float>(), mSumValue.host<float>(), threadNum);
            continue;
        }
        MNNUnpackC4(outputData, inputData, areaInput, inputTensor->channel());
        _softmaxCommon(outputData, tempData, inside, outside, channel, mMaxValue.host<float>(), mSumValue.host<float>(), threadNum);
        MNNPackC4(outputData, tempData, areaInput, outputTensor->channel());
    }

    return NO_ERROR;
}

CPUSoftmax::CPUSoftmax(Backend *b, int axis) : MNN::Execution(b), mAxis(axis), mStorage(2), mNeedUnpackC4(false) {
    // nothing to do
}

class CPUSoftmaxCreator : public CPUBackend::Creator {
public:
    virtual Execution *onCreate(const std::vector<Tensor *> &inputs, const std::vector<Tensor *> &outputs,
                                const MNN::Op *op, Backend *backend) const override {
        auto axis = op->main_as_Axis()->axis();
        if (axis < 0) {
            axis = inputs[0]->dimensions() + axis;
        }
        return new CPUSoftmax(backend, axis);
    }
};

REGISTER_CPU_OP_CREATOR(CPUSoftmaxCreator, OpType_Softmax);

} // namespace MNN
