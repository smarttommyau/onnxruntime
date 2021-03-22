// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/sequential_executor.h"

#include <chrono>
#include <thread>
#include <vector>
#include <sstream>
#include "core/common/common.h"
#include "core/common/logging/logging.h"
#include "core/framework/allocation_planner.h"
#include "core/framework/execution_frame.h"
#include "core/framework/session_state.h"
#include "core/framework/op_kernel_context_internal.h"
#include "core/framework/utils.h"
#include "core/session/inference_session.h"

#if defined DEBUG_NODE_INPUTS_OUTPUTS
#include "core/framework/debug_node_inputs_outputs_utils.h"
#endif

#ifdef ENABLE_NVTX_PROFILE
// This header is for profile using Nvidia's visual profilier.
#include "core/profile/profile.h"
#include "core/profile/context.h"
#endif

// #define TRACE_EXECUTION

// Define this symbol to create Concurrency Visualizer markers.
// See https://docs.microsoft.com/en-us/visualstudio/profiling/concurrency-visualizer-sdk
// You will need to install Concurrency Visualizer and add the SDK to the project that compiles this file
// via Analyze->Concurrency Visualizer->Add SDK to Project...
// #define CONCURRENCY_VISUALIZER
#ifdef CONCURRENCY_VISUALIZER
#include <cvmarkersobj.h>
using namespace Concurrency;
#endif

#ifdef ONNXRUNTIME_ENABLE_INSTRUMENT
#include <Windows.h>
#include "core/platform/tracing.h"
namespace {
LARGE_INTEGER OrtGetPerformanceFrequency() {
  LARGE_INTEGER v;
  // On systems that run Windows XP or later, the QueryPerformanceFrequency function will always succeed
  // and will thus never return zero.
  (void)QueryPerformanceFrequency(&v);
  return v;
}

LARGE_INTEGER perf_freq = OrtGetPerformanceFrequency();
}  // namespace
#endif

namespace onnxruntime {

static void CalculateTotalOutputSizes(OpKernelContextInternal* op_kernel_context,
                                      size_t& total_output_sizes, const std::string& node_name) {
  // Calculate total output sizes for this operation.
  total_output_sizes = 0;
  ORT_UNUSED_PARAMETER(node_name);
  for (auto i = 0; i < op_kernel_context->OutputCount(); i++) {
    const OrtValue* p_output = op_kernel_context->GetOutputMLValue(i);
    if (p_output != nullptr && p_output->IsTensor()) {
      const auto& tensor = p_output->Get<Tensor>();
      size_t tensor_size = tensor.SizeInBytes();
#if defined(TRACE_EXECUTION)
      const TensorShape& tensor_shape = tensor.Shape();
      std::cout << node_name << " output[" << i << "]"
                << " size=" << tensor_size
                << " shape=" << tensor_shape.ToString()
                << " element_size=" << tensor.DataType()->Size()
                << "\n";
#endif
      total_output_sizes += tensor_size;
    }
  }
}

static void CalculateTotalInputSizes(const OpKernelContextInternal* op_kernel_context,
                                     const onnxruntime::OpKernel* p_op_kernel,
                                     size_t& input_activation_sizes, size_t& input_parameter_sizes,
                                     const std::string& node_name) {
  // Calculate total input sizes for this operation.
  input_activation_sizes = 0;
  input_parameter_sizes = 0;
  ORT_UNUSED_PARAMETER(node_name);
  const int input_count = op_kernel_context->InputCount();
  for (auto i = 0; i < input_count; i++) {
    const OrtValue* p_input = op_kernel_context->GetInputMLValue(i);
    if (p_input != nullptr && p_input->IsTensor()) {
      const OpKernelInfo& op_kernel_info = p_op_kernel->Info();
      const Tensor* p_tensor = nullptr;
      bool is_param = op_kernel_info.TryGetConstantInput(i, &p_tensor);
      if (!is_param) {
        p_tensor = &(p_input->Get<Tensor>());
      }
      size_t tensor_size = p_tensor->SizeInBytes();

#if defined(TRACE_EXECUTION)
      const TensorShape& tensor_shape = p_tensor->Shape();
      size_t element_size = p_tensor->DataType()->Size();
      LOGS(logger, INFO) << node_name << " input[" << i << "]"
                         << " is_param=" << is_param
                         << " size=" << tensor_size
                         << " shape=" << tensor_shape.ToString()
                         << " element_size=" << element_size
                         << "\n";
#endif
      if (is_param) {
        input_parameter_sizes += tensor_size;
      } else {
        input_activation_sizes += tensor_size;
      }
    }
  }
}

static Status ReleaseNodeMLValues(ExecutionFrame& frame,
                                  const SequentialExecutionPlan& seq_exec_plan,
                                  const SequentialExecutionPlan::NodeExecutionPlan& node_exec_plan,
                                  const logging::Logger& logger);

Status SequentialExecutor::Execute(const SessionState& session_state, const std::vector<int>& feed_mlvalue_idxs,
                                   const std::vector<OrtValue>& feeds, const std::vector<int>& fetch_mlvalue_idxs,
                                   std::vector<OrtValue>& fetches,
                                   const std::unordered_map<size_t, IExecutor::CustomAllocator>& fetch_allocators,
                                   const logging::Logger& logger,
                                   int64_t& run_id) {
  const SequentialExecutionPlan& seq_exec_plan = *session_state.GetExecutionPlan();
  const auto& exec_plan_vec = seq_exec_plan.execution_plan;
  const auto exec_plan_size = exec_plan_vec.size();
  if (run_id == DEFAULT_RUN_ID) {
    ExecutionFrame frame{feed_mlvalue_idxs, feeds, fetch_mlvalue_idxs, fetches, fetch_allocators, session_state};
    ORT_RETURN_IF_ERROR(Execute(session_state, feeds, fetch_mlvalue_idxs, logger, frame, 0, exec_plan_size));
    VLOGS(logger, 1) << "Fetching output.";
    // ExecutionFrame::Finalize will update 'fetches' with the final output
    ORT_RETURN_IF_ERROR(frame.GetOutputs(fetches, session_state.GetTransferIntermidiateTensorOwnership()));
    VLOGS(logger, 1) << "Done with execution.";
  } else {
    // Partial graph execution frame management code.
    ExecutionFrame* frame;
    std::map<int64_t, std::unique_ptr<onnxruntime::ExecutionFrame>>::iterator it;
    if (run_id == DEFAULT_PARTIAL_RUN_ID) {
      auto p_frame = onnxruntime::make_unique<ExecutionFrame>(feed_mlvalue_idxs, feeds, fetch_mlvalue_idxs, fetches,
                                                              fetch_allocators, session_state);

      frame = p_frame.get();
      std::lock_guard<OrtMutex> lock(session_state.graph_runs_lock_);
      it = session_state.graph_runs_.insert(std::make_pair(session_state.graph_runs_counter_, std::move(p_frame))).first;
      ORT_ENFORCE(run_id < INT64_MAX);
      run_id = session_state.graph_runs_counter_;
      // REVIEW(mzs): May be reuse counters and prevent a potential overflow?
      session_state.graph_runs_counter_ += 1;
    } else {
      ORT_ENFORCE(run_id >= 0);

      it = session_state.graph_runs_.find(run_id);

      ORT_ENFORCE(it != session_state.graph_runs_.end());

      (it->second)->UpdateFeedAndFetches(feed_mlvalue_idxs, feeds, const_cast<std::vector<int>&>(fetch_mlvalue_idxs), fetches);
      frame = (it->second).get();
    }

    // Determine partial graph execution start and end nodes.
    // REVIEW(codemzs): Ideally these should be determined just by looking at fetches but we can make that
    // optimization after the first commit. That will also get rid of the need for an "explicit" operator to indicate
    // partial graph execution cut point, this will require some thinking on making sure the control flow of execution order
    // is correct, i.e backward pass follows forward pass.
    size_t program_counter_end;
    for (program_counter_end = frame->program_counter_;
         (program_counter_end < exec_plan_size) &&
         ((session_state.GetKernel(exec_plan_vec[program_counter_end].node_index)->KernelDef().OpName() != "YieldOp") ||
          (program_counter_end == frame->program_counter_));
         program_counter_end += 1)
      ;

    ORT_RETURN_IF_ERROR(Execute(session_state, feeds, fetch_mlvalue_idxs, logger, *frame, frame->program_counter_, program_counter_end));

    // Make sure intermediate outputs are ready in the event they are being asynchronously computed.
    if (program_counter_end < exec_plan_size) {
      const SequentialExecutionPlan& seq_exec_plan = *session_state.GetExecutionPlan();
      const auto& exec_plan_vec = seq_exec_plan.execution_plan;
      const auto& node_exec_plan = exec_plan_vec[program_counter_end];
      auto node_index = node_exec_plan.node_index;
      const auto& graph_viewer = session_state.GetGraphViewer();
      const auto& node = *graph_viewer.GetNode(node_exec_plan.node_index);
      auto p_op_kernel = session_state.GetKernel(node_index);
      if (p_op_kernel == nullptr)
        return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Got nullptr from GetKernel for node: ",
                               node.Name());

      OpKernelContextInternal op_kernel_context(session_state, *frame, *p_op_kernel, logger, terminate_flag_);
      int queue_id = p_op_kernel->KernelDef().ExecQueueId();
      if (seq_exec_plan.NodeHasFence(node_index)) {
        for (int input_index = 0; input_index < op_kernel_context.InputCount(); ++input_index) {
          Fence_t fence = op_kernel_context.InputFence(input_index);
          if (fence) {
            auto execution_provider_type = p_op_kernel->Node().GetExecutionProviderType();
            if (OrtMemTypeCPUInput == p_op_kernel->KernelDef().InputMemoryType(input_index)) {
              execution_provider_type = kCpuExecutionProvider;
            }
            fence->BeforeUsingAsInput(execution_provider_type, queue_id);
          }
        }

        for (int input_index = 0; input_index < op_kernel_context.ImplicitInputCount(); ++input_index) {
          Fence_t fence = op_kernel_context.ImplicitInputFence(input_index);
          if (fence) {
            auto execution_provider_type = p_op_kernel->Node().GetExecutionProviderType();
            if (OrtMemTypeCPUInput == p_op_kernel->KernelDef().InputMemoryType(input_index)) {
              execution_provider_type = kCpuExecutionProvider;
            }
            fence->BeforeUsingAsInput(execution_provider_type, queue_id);
          }
        }

        for (int output_index = 0; output_index < op_kernel_context.OutputCount(); ++output_index) {
          Fence_t fence = op_kernel_context.OutputFence(output_index);
          if (fence) {
            fence->BeforeUsingAsOutput(p_op_kernel->Node().GetExecutionProviderType(), queue_id);
          }
        }
      }
    }

    VLOGS(logger, 1) << "Fetching output.";
    // ExecutionFrame::Finalize will update 'fetches' with the final output
    ORT_RETURN_IF_ERROR(frame->GetOutputs(fetches, session_state.GetTransferIntermidiateTensorOwnership()));
    VLOGS(logger, 1) << "Done with execution.";

    // Set the program counter to the begining of next partial run depending upon the need to free the tensors.
    if (session_state.GetTransferIntermidiateTensorOwnership()) {
      frame->program_counter_ = program_counter_end + 1;
    } else {
      frame->program_counter_ = program_counter_end;
    }

    // Release the frame upon the completion of "full" graph execution.
    if (frame->program_counter_ == exec_plan_size) {
      std::lock_guard<OrtMutex> lock(session_state.graph_runs_lock_);
      session_state.graph_runs_.erase(it);
    }
  }

  return Status::OK();
}

Status SequentialExecutor::Execute(const SessionState& session_state,
                                   const std::vector<OrtValue>& feeds, const std::vector<int>& fetch_mlvalue_idxs,
                                   const logging::Logger& logger, ExecutionFrame& frame, size_t program_counter_start,
                                   size_t program_counter_end) {
  const bool is_profiler_enabled = session_state.Profiler().IsEnabled();
  TimePoint tp;
  TimePoint sync_time_begin;
  TimePoint kernel_begin_time;
  size_t input_activation_sizes = 0;
  size_t input_parameter_sizes = 0;
  size_t total_output_sizes = 0;

  if (is_profiler_enabled) {
    tp = session_state.Profiler().StartTime();
  }

  const std::unordered_set<NodeIndex>* to_be_executed_nodes = nullptr;

#if !defined(ORT_MINIMAL_BUILD)

  to_be_executed_nodes = session_state.GetToBeExecutedNodes(fetch_mlvalue_idxs);
  const bool only_execute_path_to_fetches = only_execute_path_to_fetches_ && (to_be_executed_nodes != nullptr);

  if (only_execute_path_to_fetches) {
    VLOGS(logger, 1) << to_be_executed_nodes->size() << " nodes to be executed\n";
  }
#else
  ORT_UNUSED_PARAMETER(fetch_mlvalue_idxs);
  ORT_UNUSED_PARAMETER(only_execute_path_to_fetches_);
  const bool only_execute_path_to_fetches = false;
#endif

  LOGS(logger, INFO) << "Begin execution";
  const SequentialExecutionPlan& seq_exec_plan = *session_state.GetExecutionPlan();
  const auto& exec_plan_vec = seq_exec_plan.execution_plan;
  VLOGS(logger, 1) << "Size of execution plan vector: " << exec_plan_vec.size();
  VLOGS(logger, 1) << "Executing from: " << program_counter_start;
  VLOGS(logger, 1) << "Executing until but not including: " << program_counter_end;

// Enable TRACE_EXECUTION compile flag to dump execution plan
#if defined(TRACE_EXECUTION)
  std::cout << std::make_pair(&seq_exec_plan, &session_state) << std::endl;
#endif

  const auto& graph_viewer = session_state.GetGraphViewer();

#ifdef CONCURRENCY_VISUALIZER
  // need unique name for the series. number of nodes should be good enough for a subgraph
  char series_name[MaxSeriesNameLengthInChars] = "MainGraph";
  if (graph_viewer->IsSubgraph()) {
    auto s = graph_viewer->ParentNode()->Name().substr(0, MaxSeriesNameLengthInChars - 1);
    std::copy(s.cbegin(), s.cend(), series_name);
  }

  diagnostic::marker_series series(series_name);
#endif

#ifdef ENABLE_NVTX_PROFILE
  auto& profile_context = profile::Context::GetInstance();
  const auto tag = profile_context.GetThreadTagOrDefault(std::this_thread::get_id());
  profile::NvtxRangeCreator forward_range(
      "Batch-" + tag + " Forward",
      profile::Color::White);
  profile::NvtxRangeCreator backward_range(
      "Batch-" + tag + " Backward",
      profile::Color::Black);
#endif

  for (size_t program_counter = program_counter_start; program_counter < program_counter_end; program_counter += 1) {
    const auto& node_exec_plan = exec_plan_vec[program_counter];
    if (terminate_flag_) {
      LOGS(logger, WARNING) << "Exiting due to terminate flag being set to true.";
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Exiting due to terminate flag being set to true.");
    }

    auto node_index = node_exec_plan.node_index;

    // If it is not necessary to execute the node.
    if (only_execute_path_to_fetches && to_be_executed_nodes->count(node_index) == 0) {
      continue;
    }

    const auto& node = *graph_viewer.GetNode(node_exec_plan.node_index);

#ifdef CONCURRENCY_VISUALIZER
    series.write_flag(node.Name().c_str());
#endif

#ifdef ENABLE_NVTX_PROFILE
    if (node.Description() != "Backward pass" && !forward_range.IsBeginCalled()) {
      // Start timing forward pass when encountering the first forward node.
      forward_range.Begin();
    } else if (node.Description() == "Backward pass" && !backward_range.IsBeginCalled() && forward_range.IsBeginCalled()) {
      // Start timing backward pass when encountering the first backward node.
      // In the meanwhile, forward range ends.
      forward_range.End();
      backward_range.Begin();
    }
#endif

    auto p_op_kernel = session_state.GetKernel(node_index);

    // if a kernel has been added in the session state, it better be NON-null.
    if (p_op_kernel == nullptr)
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Got nullptr from GetKernel for node: ",
                             node.Name());

#ifdef ONNXRUNTIME_ENABLE_INSTRUMENT
    LARGE_INTEGER kernel_start;
    QueryPerformanceCounter(&kernel_start);
#endif
    // construct OpKernelContext
    // TODO: log kernel inputs?
    OpKernelContextInternal op_kernel_context(session_state, frame, *p_op_kernel, logger, terminate_flag_);
    // TODO: log kernel outputs?
    if (is_profiler_enabled) {
      sync_time_begin = session_state.Profiler().StartTime();
    }

    // sync before compute
    int queue_id = p_op_kernel->KernelDef().ExecQueueId();
    if (seq_exec_plan.NodeHasFence(node_index)) {
      for (int input_index = 0; input_index < op_kernel_context.InputCount(); ++input_index) {
        Fence_t fence = op_kernel_context.InputFence(input_index);
        if (fence) {
          auto execution_provider_type = p_op_kernel->Node().GetExecutionProviderType();
          if (OrtMemTypeCPUInput == p_op_kernel->KernelDef().InputMemoryType(input_index)) {
            execution_provider_type = kCpuExecutionProvider;
          }
          fence->BeforeUsingAsInput(execution_provider_type, queue_id);
        }
      }

      for (int input_index = 0; input_index < op_kernel_context.ImplicitInputCount(); ++input_index) {
        Fence_t fence = op_kernel_context.ImplicitInputFence(input_index);
        if (fence) {
          auto execution_provider_type = p_op_kernel->Node().GetExecutionProviderType();
          if (OrtMemTypeCPUInput == p_op_kernel->KernelDef().InputMemoryType(input_index)) {
            execution_provider_type = kCpuExecutionProvider;
          }
          fence->BeforeUsingAsInput(execution_provider_type, queue_id);
        }
      }

      for (int output_index = 0; output_index < op_kernel_context.OutputCount(); ++output_index) {
        Fence_t fence = op_kernel_context.OutputFence(output_index);
        if (fence) {
          fence->BeforeUsingAsOutput(p_op_kernel->Node().GetExecutionProviderType(), queue_id);
        }
      }
    }
#ifdef DEBUG_NODE_INPUTS_OUTPUTS
    utils::DumpNodeInputs(op_kernel_context, p_op_kernel->Node(), session_state);
#endif

    const std::string node_name_for_profiling = [&]() -> std::string {
      if (!is_profiler_enabled) return {};
      // Derive something meaningful for profile traces and logs if node name field is blank in execution graph
      return node.Name().empty() ? MakeString(node.OpType(), "_", node_index) : node.Name();
    }();

    if (is_profiler_enabled) {
      session_state.Profiler().EndTimeAndRecordEvent(profiling::NODE_EVENT,
                                                     node_name_for_profiling + "_fence_before",
                                                     sync_time_begin,
                                                     {{"op_name", p_op_kernel->KernelDef().OpName()}});
      concurrency::ThreadPool::StartProfiling(session_state.GetThreadPool());
      // call compute on the kernel
      VLOGS(logger, 1) << "Computing kernel: " << node_name_for_profiling;

      kernel_begin_time = session_state.Profiler().StartTime();

      // Calculate total input sizes for this operation.
      CalculateTotalInputSizes(&op_kernel_context, p_op_kernel,
                               input_activation_sizes, input_parameter_sizes, node_name_for_profiling);
    }

    Status compute_status;
    {
#ifdef CONCURRENCY_VISUALIZER
      diagnostic::span span(series, "%s.%d", node.OpType().c_str(), node.Index());
#endif
#ifdef ENABLE_NVTX_PROFILE
      profile::NvtxRangeCreator node_compute_range(
          MakeString(node.OpType(), ".", node.Index(), "(", node.Name(), ")"), profile::Color::Yellow);
      node_compute_range.Begin();
#endif
      ORT_TRY {
#ifdef ENABLE_TRAINING
        if (p_op_kernel->KernelDef().AllocateInputsContiguously()) {
          ORT_RETURN_IF_ERROR(utils::VerifyInputTensorsAllocatedContiguously(&op_kernel_context));
        }
#endif

        compute_status = p_op_kernel->Compute(&op_kernel_context);
      }
      ORT_CATCH(const std::exception& ex) {
        ORT_HANDLE_EXCEPTION([&]() {
          compute_status = ORT_MAKE_STATUS(ONNXRUNTIME, RUNTIME_EXCEPTION, ex.what());
        });
      }

#ifdef ENABLE_NVTX_PROFILE
      node_compute_range.End();
#endif
    }

    if (!compute_status.IsOK()) {
      std::ostringstream ss;
      ss << "Non-zero status code returned while running " << node.OpType() << " node. Name:'" << node.Name()
         << "' Status Message: " << compute_status.ErrorMessage();
      //If the computation failed, we still can record the memory consumption
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
      MemoryInfo::MemoryInfoProfile::CreateEvents("dynamic activations_" + std::to_string(MemoryInfo::GetIteration()),
                                                  MemoryInfo::MemoryInfoProfile::GetAndIncreasePid(), MemoryInfo::MapType::DynamicActivation, "", 0);
#endif
      const auto msg_string = ss.str();
      LOGS(logger, ERROR) << msg_string;
      return Status(compute_status.Category(), compute_status.Code(), msg_string);
    }

    if (is_profiler_enabled) {
      // Calculate total output sizes for this operation.
      CalculateTotalOutputSizes(&op_kernel_context, total_output_sizes, node_name_for_profiling);

#if defined(TRACE_EXECUTION)
      // Trace execution step.
      const Node& node = p_op_kernel->Node();
      std::cout << "Executed op kernel node " << node_name_for_profiling
                << " Index=" << node.Index()
                << " OpType=" << node.OpType()
                << " Name=" << node.Name()
                << " Activation_Size=" << input_activation_sizes
                << " Parameter_Size=" << input_parameter_sizes
                << " Output_Size=" << total_output_sizes
                << "\n";
#endif

      session_state.Profiler().EndTimeAndRecordEvent(profiling::NODE_EVENT,
                                                     node_name_for_profiling + "_kernel_time",
                                                     kernel_begin_time,
                                                     // Log additional operation args / info.
                                                     {
                                                         {"op_name", p_op_kernel->KernelDef().OpName()},
                                                         {"provider", p_op_kernel->KernelDef().Provider()},
                                                         {"graph_index", std::to_string(p_op_kernel->Node().Index())},
                                                         {"exec_plan_index", std::to_string(node_index)},
                                                         {"activation_size", std::to_string(input_activation_sizes)},
                                                         {"parameter_size", std::to_string(input_parameter_sizes)},
                                                         {"output_size", std::to_string(total_output_sizes)},
                                                         {"thread_scheduling_stats", concurrency::ThreadPool::StopProfiling(session_state.GetThreadPool())},
                                                     });
      sync_time_begin = session_state.Profiler().StartTime();
    }

    // sync after compute for outputs
    if (seq_exec_plan.NodeHasFence(node_index)) {
      for (int input_index = 0; input_index < op_kernel_context.InputCount(); ++input_index) {
        Fence_t fence = op_kernel_context.InputFence(input_index);
        if (fence) {
          fence->AfterUsedAsInput(queue_id);
        }
      }

      for (int input_index = 0; input_index < op_kernel_context.ImplicitInputCount(); ++input_index) {
        Fence_t fence = op_kernel_context.ImplicitInputFence(input_index);
        if (fence) {
          fence->AfterUsedAsInput(queue_id);
        }
      }

      for (int output_index = 0; output_index < op_kernel_context.OutputCount(); ++output_index) {
        Fence_t fence = op_kernel_context.OutputFence(output_index);
        if (fence) {
          fence->AfterUsedAsOutput(queue_id);
        }
      }
    }
#ifdef ONNXRUNTIME_ENABLE_INSTRUMENT
    LARGE_INTEGER kernel_stop;
    QueryPerformanceCounter(&kernel_stop);
    LARGE_INTEGER elapsed;
    elapsed.QuadPart = kernel_stop.QuadPart - kernel_start.QuadPart;
    elapsed.QuadPart *= 1000000;
    elapsed.QuadPart /= perf_freq.QuadPart;
    // Log an event
    TraceLoggingWrite(telemetry_provider_handle,  // handle to my provider
                      "OpEnd",                    // Event Name that should uniquely identify your event.
                      TraceLoggingValue(p_op_kernel->KernelDef().OpName().c_str(), "op_name"),
                      TraceLoggingValue(elapsed.QuadPart, "time"));
#endif
    if (is_profiler_enabled) {
      session_state.Profiler().EndTimeAndRecordEvent(profiling::NODE_EVENT,
                                                     node_name_for_profiling + "_fence_after",
                                                     sync_time_begin,
                                                     {{"op_name", p_op_kernel->KernelDef().OpName()}});
    }

#ifdef DEBUG_NODE_INPUTS_OUTPUTS
    utils::DumpNodeOutputs(op_kernel_context, p_op_kernel->Node(), session_state);
#endif

    // free ml-values corresponding to this node
    VLOGS(logger, 1) << "Releasing node ML values.";
    ORT_RETURN_IF_ERROR(ReleaseNodeMLValues(frame, seq_exec_plan, node_exec_plan, logger));
  }

#ifdef ENABLE_NVTX_PROFILE
  // Make sure forward Range object call Begin and End.
  if (!forward_range.IsBeginCalled()) {
    forward_range.Begin();
  }
  if (!forward_range.IsEndCalled()) {
    forward_range.End();
  }
  // Make sure backward Range object call Begin and End.
  if (!backward_range.IsBeginCalled()) {
    backward_range.Begin();
  }
  if (!backward_range.IsEndCalled()) {
    backward_range.End();
  }
#endif

#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
  MemoryInfo::MemoryInfoProfile::CreateEvents("dynamic activations_" + std::to_string(MemoryInfo::GetIteration()),
                                              MemoryInfo::MemoryInfoProfile::GetAndIncreasePid(), MemoryInfo::MapType::DynamicActivation, "", 0);
  MemoryInfo::MemoryInfoProfile::Clear();
#endif

  if (frame.HasMemoryPatternPlanner()) {
    std::vector<std::reference_wrapper<const TensorShape>> input_shapes;
    bool all_tensors = true;
    for (const auto& feed : feeds) {
      if (!(feed.IsTensor())) {
        all_tensors = false;
        break;
      }
      auto& tensor = feed.Get<Tensor>();
      input_shapes.push_back(std::cref(tensor.Shape()));
    }

    if (all_tensors) {
      auto mem_patterns = onnxruntime::make_unique<MemoryPatternGroup>();
      ORT_RETURN_IF_ERROR(frame.GeneratePatterns(mem_patterns.get()));
      ORT_RETURN_IF_ERROR(session_state.UpdateMemoryPatternGroupCache(input_shapes, std::move(mem_patterns)));
    }
  }

  if (is_profiler_enabled) {
    session_state.Profiler().EndTimeAndRecordEvent(profiling::SESSION_EVENT, "SequentialExecutor::Execute", tp);
  }

  for (auto i : frame.GetStaticMemorySizeInfo()) {
    LOGS(logger, INFO) << "[Memory] ExecutionFrame statically allocates "
                       << i.second << " bytes for " << i.first << std::endl;
  }

  for (auto i : frame.GetDynamicMemorySizeInfo()) {
    LOGS(logger, INFO) << "[Memory] ExecutionFrame dynamically allocates "
                       << i.second << " bytes for " << i.first << std::endl;
  }

  return Status::OK();
}

static Status ReleaseNodeMLValues(ExecutionFrame& frame,
                                  const SequentialExecutionPlan& seq_exec_plan,
                                  const SequentialExecutionPlan::NodeExecutionPlan& node_exec_plan,
                                  const logging::Logger& logger) {
  for (auto i = node_exec_plan.free_from_index; i <= node_exec_plan.free_to_index; ++i) {
    auto ort_value_idx = seq_exec_plan.to_be_freed[i];
    VLOGS(logger, 1) << "Releasing ort_value with index: " << ort_value_idx;
    ORT_RETURN_IF_ERROR(frame.ReleaseMLValue(ort_value_idx));
  }

  return Status::OK();
}
}  // namespace onnxruntime
