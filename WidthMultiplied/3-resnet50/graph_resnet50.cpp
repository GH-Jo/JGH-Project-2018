/*
 * Copyright (c) 2017-2018 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/graph.h"
#include "arm_compute/runtime/CL/CLScheduler.h"
#include "support/ToolchainSupport.h"
#include "utils/CommonGraphOptions.h"
#include "utils/GraphUtils.h"
#include "utils/Utils.h"
#include <streamline_annotate.h>
#include <time.h>
#include <cstdio>

using namespace arm_compute;
using namespace arm_compute::utils;
using namespace arm_compute::graph::frontend;
using namespace arm_compute::graph_utils;
std::unique_ptr<arm_compute::graph::ITensorAccessor> Dummy() {
				    return arm_compute::support::cpp14::make_unique<DummyAccessor>(1);
}

/** Example demonstrating how to implement ResNet50 network using the Compute Library's graph API
 *
 * @param[in] argc Number of arguments
 * @param[in] argv Arguments
 */
class GraphResNet50Example : public Example
{
public:
    GraphResNet50Example()
        : cmd_parser(), common_opts(cmd_parser), common_params(), graph(0, "ResNet50")
    {
    }
    bool do_setup(int argc, char **argv) override
    {
        // Parse arguments
        cmd_parser.parse(argc, argv);

        // Consume common parameters
        common_params = consume_common_graph_parameters(common_opts);

        // Return when help menu is requested
        if(common_params.help)
        {
            cmd_parser.print_help(argv[0]);
            return false;
        }

        // Checks
        ARM_COMPUTE_EXIT_ON_MSG(arm_compute::is_data_type_quantized_asymmetric(common_params.data_type), "QASYMM8 not supported for this graph");
        ARM_COMPUTE_EXIT_ON_MSG(common_params.data_type == DataType::F16 && common_params.target == Target::NEON, "F16 NEON not supported for this graph");

        // Print parameter values
        std::cout << common_params << std::endl;

        // Get trainable parameters data path
        std::string data_path = common_params.data_path;

        // Create a preprocessor object
        const std::array<float, 3> mean_rgb{ { 122.68f, 116.67f, 104.01f } };
        std::unique_ptr<IPreprocessor> preprocessor = arm_compute::support::cpp14::make_unique<CaffePreproccessor>(mean_rgb,
                                                                                                                   false /* Do not convert to BGR */);

        // Create input descriptor
        const TensorShape tensor_shape     = permute_shape(TensorShape(224U, 224U, 3U, 1U), DataLayout::NCHW, common_params.data_layout);
        TensorDescriptor  input_descriptor = TensorDescriptor(tensor_shape, common_params.data_type).set_layout(common_params.data_layout);

        // Set weights trained layout
        const DataLayout weights_layout = DataLayout::NCHW;
        float depth_scale = 1.f;

        graph << common_params.target
              << common_params.fast_math_hint
              << InputLayer(input_descriptor, get_input_accessor(common_params, std::move(preprocessor), false /* Do not convert to BGR */))
              << ConvolutionLayer(
                  7U, 7U, 64U* depth_scale,
                  Dummy(),//get_weights_accessor(data_path, "/cnn_data/resnet50_model/conv1_weights.npy", weights_layout),
                  std::unique_ptr<arm_compute::graph::ITensorAccessor>(nullptr),
                  PadStrideInfo(2, 2, 3, 3))
              .set_name("conv1/convolution")
              << BatchNormalizationLayer(
                  Dummy(),//get_weights_accessor(data_path, "/cnn_data/resnet50_model/conv1_BatchNorm_moving_mean.npy"),
                  Dummy(),//get_weights_accessor(data_path, "/cnn_data/resnet50_model/conv1_BatchNorm_moving_variance.npy"),
                  Dummy(),//get_weights_accessor(data_path, "/cnn_data/resnet50_model/conv1_BatchNorm_gamma.npy"),
                  Dummy(),//get_weights_accessor(data_path, "/cnn_data/resnet50_model/conv1_BatchNorm_beta.npy"),
                  0.0000100099996416f)
              .set_name("conv1/BatchNorm")
              << ActivationLayer(ActivationLayerInfo(ActivationLayerInfo::ActivationFunction::RELU)).set_name("conv1/Relu")
              << PoolingLayer(PoolingLayerInfo(PoolingType::MAX, 3, PadStrideInfo(2, 2, 0, 1, 0, 1, DimensionRoundingType::FLOOR))).set_name("pool1/MaxPool");

        add_residual_block(data_path, "block1", weights_layout, 64* depth_scale, 3, 2);
        add_residual_block(data_path, "block2", weights_layout, 128* depth_scale, 4, 2);
        add_residual_block(data_path, "block3", weights_layout, 256* depth_scale, 6, 2);
        add_residual_block(data_path, "block4", weights_layout, 512* depth_scale, 3, 1);

        graph << PoolingLayer(PoolingLayerInfo(PoolingType::AVG)).set_name("pool5")
              << ConvolutionLayer(
                  1U, 1U, 1000U,
                  Dummy(),//get_weights_accessor(data_path, "/cnn_data/resnet50_model/logits_weights.npy", weights_layout),
                  Dummy(),//get_weights_accessor(data_path, "/cnn_data/resnet50_model/logits_biases.npy"),
                  PadStrideInfo(1, 1, 0, 0))
              .set_name("logits/convolution")
              << FlattenLayer().set_name("predictions/Reshape")
              << SoftmaxLayer().set_name("predictions/Softmax")
              << OutputLayer(get_output_accessor(common_params, 5));

        // Finalize graph
        GraphConfig config;
        config.num_threads = common_params.threads;
        config.use_tuner   = common_params.enable_tuner;
        graph.finalize(common_params.target, config);

        return true;
    }
    void do_run() override
    {
        // Run graph
				//struct timespec t0, t1;
				//float t;
				ANNOTATE_SETUP;
				ANNOTATE_DEFINE;
				for (int i=0; i<10; i++)  // warming up 
								graph.run();
				ANNOTATE("graph-run-start");
				//clock_gettime(CLOCK_REALTIME, &t0);
				for (int i=0; i<20; i++)
        	graph.run();
				CLScheduler::get().sync();
				//clock_gettime(CLOCK_REALTIME, &t1);
				ANNOTATE("graph-run-end");
				//t = (float)(t1.tv_sec-t0.tv_sec)+(float)(t1.tv_nsec-t0.tv_nsec)/1e9;
				//printf("Time spent: %.4f\n",t);
    }

private:
    CommandLineParser  cmd_parser;
    CommonGraphOptions common_opts;
    CommonGraphParams  common_params;
    Stream             graph;

    void add_residual_block(const std::string &data_path, const std::string &name, DataLayout weights_layout,
                            unsigned int base_depth, unsigned int num_units, unsigned int stride)
    {
        for(unsigned int i = 0; i < num_units; ++i)
        {
            std::stringstream unit_path_ss;
            unit_path_ss << "/cnn_data/resnet50_model/" << name << "_unit_" << (i + 1) << "_bottleneck_v1_";
            std::stringstream unit_name_ss;
            unit_name_ss << name << "/unit" << (i + 1) << "/bottleneck_v1/";

            std::string unit_path = unit_path_ss.str();
            std::string unit_name = unit_name_ss.str();

            unsigned int middle_stride = 1;

            if(i == (num_units - 1))
            {
                middle_stride = stride;
            }

            SubStream right(graph);
            right << ConvolutionLayer(
                      1U, 1U, base_depth,
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv1_weights.npy", weights_layout),
                      std::unique_ptr<arm_compute::graph::ITensorAccessor>(nullptr),
                      PadStrideInfo(1, 1, 0, 0))
                  .set_name(unit_name + "conv1/convolution")
                  << BatchNormalizationLayer(
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv1_BatchNorm_moving_mean.npy"),
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv1_BatchNorm_moving_variance.npy"),
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv1_BatchNorm_gamma.npy"),
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv1_BatchNorm_beta.npy"),
                      0.0000100099996416f)
                  .set_name(unit_name + "conv1/BatchNorm")
                  << ActivationLayer(ActivationLayerInfo(ActivationLayerInfo::ActivationFunction::RELU)).set_name(unit_name + "conv1/Relu")

                  << ConvolutionLayer(
                      3U, 3U, base_depth,
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv2_weights.npy", weights_layout),
                      std::unique_ptr<arm_compute::graph::ITensorAccessor>(nullptr),
                      PadStrideInfo(middle_stride, middle_stride, 1, 1))
                  .set_name(unit_name + "conv2/convolution")
                  << BatchNormalizationLayer(
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv2_BatchNorm_moving_mean.npy"),
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv2_BatchNorm_moving_variance.npy"),
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv2_BatchNorm_gamma.npy"),
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv2_BatchNorm_beta.npy"),
                      0.0000100099996416f)
                  .set_name(unit_name + "conv2/BatchNorm")
                  << ActivationLayer(ActivationLayerInfo(ActivationLayerInfo::ActivationFunction::RELU)).set_name(unit_name + "conv1/Relu")

                  << ConvolutionLayer(
                      1U, 1U, base_depth * 4,
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv3_weights.npy", weights_layout),
                      std::unique_ptr<arm_compute::graph::ITensorAccessor>(nullptr),
                      PadStrideInfo(1, 1, 0, 0))
                  .set_name(unit_name + "conv3/convolution")
                  << BatchNormalizationLayer(
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv3_BatchNorm_moving_mean.npy"),
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv3_BatchNorm_moving_variance.npy"),
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv3_BatchNorm_gamma.npy"),
                      Dummy(),//get_weights_accessor(data_path, unit_path + "conv3_BatchNorm_beta.npy"),
                      0.0000100099996416f)
                  .set_name(unit_name + "conv2/BatchNorm");

            if(i == 0)
            {
                SubStream left(graph);
                left << ConvolutionLayer(
                         1U, 1U, base_depth * 4,
                         Dummy(),//get_weights_accessor(data_path, unit_path + "shortcut_weights.npy", weights_layout),
                         std::unique_ptr<arm_compute::graph::ITensorAccessor>(nullptr),
                         PadStrideInfo(1, 1, 0, 0))
                     .set_name(unit_name + "shortcut/convolution")
                     << BatchNormalizationLayer(
                         Dummy(),//get_weights_accessor(data_path, unit_path + "shortcut_BatchNorm_moving_mean.npy"),
                         Dummy(),//get_weights_accessor(data_path, unit_path + "shortcut_BatchNorm_moving_variance.npy"),
                         Dummy(),//get_weights_accessor(data_path, unit_path + "shortcut_BatchNorm_gamma.npy"),
                         Dummy(),//get_weights_accessor(data_path, unit_path + "shortcut_BatchNorm_beta.npy"),
                         0.0000100099996416f)
                     .set_name(unit_name + "shortcut/BatchNorm");

                graph << BranchLayer(BranchMergeMethod::ADD, std::move(left), std::move(right)).set_name(unit_name + "add");
            }
            else if(middle_stride > 1)
            {
                SubStream left(graph);
                left << PoolingLayer(PoolingLayerInfo(PoolingType::MAX, 1, PadStrideInfo(middle_stride, middle_stride, 0, 0), true)).set_name(unit_name + "shortcut/MaxPool");

                graph << BranchLayer(BranchMergeMethod::ADD, std::move(left), std::move(right)).set_name(unit_name + "add");
            }
            else
            {
                SubStream left(graph);
                graph << BranchLayer(BranchMergeMethod::ADD, std::move(left), std::move(right)).set_name(unit_name + "add");
            }

            graph << ActivationLayer(ActivationLayerInfo(ActivationLayerInfo::ActivationFunction::RELU)).set_name(unit_name + "Relu");
        }
    }
};

/** Main program for ResNet50
 *
 * @note To list all the possible arguments execute the binary appended with the --help option
 *
 * @param[in] argc Number of arguments
 * @param[in] argv Arguments
 */
int main(int argc, char **argv)
{
    return arm_compute::utils::run_example<GraphResNet50Example>(argc, argv);
}
