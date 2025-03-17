#include <sycl/sycl.hpp>
using namespace sycl;

//////////////////////////////////////////////////////////////////////////////
// SYCL boilder plate
//////////////////////////////////////////////////////////////////////////////

#if FPGA_EMULATOR || FPGA_HARDWARE || FPGA_SIMULATOR
#include <sycl/ext/intel/fpga_extensions.hpp>
#endif

//////////////////////////////////////////////////////////////////////////////
// Kernel exception handler. Just rethrows exceptions and terminates.
//////////////////////////////////////////////////////////////////////////////

static auto exception_handler = [](sycl::exception_list e_list) {
  for (std::exception_ptr const &e : e_list) {
    try {
      std::rethrow_exception(e);
    } catch (std::exception const &e) {
#if _DEBUG
      std::cout << "Failure" << std::endl;
#endif
      std::terminate();
    }
  }
};

//////////////////////////////////////////////////////////////////////////////
// Declare stream element and flit type
//////////////////////////////////////////////////////////////////////////////

// typedef double Element;
typedef uint64_t Element;
// typedef uint16_t Element;

// handle multiple elements per cycle for concurrency
#define STRM_WIDTH ((int)(64 / sizeof(Element)))
typedef struct {
  Element element[STRM_WIDTH];
} Flit;

//////////////////////////////////////////////////////////////////////////////
// Declare Streaming Pipes
//
// Note: in SYCL, pipes are "types" with fixed reader and
// writers. Pipes cannot be passed around as variables.
//
// Programmer must set minimum pipe depth to avoid deadlock. Compiler
// will adjust depth for performance.
//////////////////////////////////////////////////////////////////////////////

#define DEFAULT_PIPE_DEPTH (4)

// from data source to kernel pipeline
using InPipe = ext::intel::pipe<class InPipe_id, Flit, DEFAULT_PIPE_DEPTH>;
// from kernel to data sink
using OutPipe = ext::intel::pipe<class outPipe_id, Flit, DEFAULT_PIPE_DEPTH>;

// The above is all you need for prefixSumSimple();
// The below are intermediate pipes between the 2 parts prefixSumA() and
// prefixSumB();
using DataPipe = ext::intel::pipe<class DataPipe_id, Flit, DEFAULT_PIPE_DEPTH>;
using SumPipe = ext::intel::pipe<class SumPipe_id, Element, DEFAULT_PIPE_DEPTH>;

//////////////////////////////////////////////////////////////////////////////
// The kernel example computes the prefix sums on the input
// stream. That is, to produce the output strea, the kernel replaces
// each element by the sum of all previous elements including itself.
//
// The kernel is writen as a function template so it is reusable with
// different input and output pipe connections.
//
// Stream kernel never terminates; this is unusal for normal
// compute-offload kernels.
//////////////////////////////////////////////////////////////////////////////

// In prefixSumSimple(), each iteration of the outermost while-loop
// depends on the final sum of the previous iteration.  To achieve the
// requested II=1, all elements in a flit must be summed together and
// added to the running sum in 1 cyc, leading to a low clock
// frequency.  The compiler cannot produce II=1 schedule for
// double-type Element since combinational cascading of FADD is not
// supported.

template <typename TInPipe, typename TOutPipe> void prefixSumSimple() {
  Element prefixSum = 0; // this is state carried across time

  [[intel::initiation_interval(1)]] // insist II=1 schedule
  while (1) {
    Flit iflit = TInPipe::read(); // get this iteration's input
    Flit oflit;                   // output flit to be prepared

    // need to unroll to initiate on a new flit each cycle
#pragma unroll
    for (int64_t i = 0; i < STRM_WIDTH; i++) {
      prefixSum += iflit.element[i];
      oflit.element[i] = prefixSum;
    }

    TOutPipe::write(oflit); // push this iteration's output
  }
}

// In this version, streaming prefix sum is computed in 2 kernels, A
// and B.  prefixSumA() computes the sum across the Elements of the
// same flit.  There is no dependency across iterations so high
// frequency is achieviable for II=1 by pipelining. prefixSumA()
// forwards both the flit and its partial sum to prefixSumB().
// prefixSumB() computes the prefixSum of each element of a flit based
// of the running sum from the last iteration. In this case, each
// iteration need to update prefixSum=prefixSum+partialSum for the
// next iteration.  Even if Element is double, II can still be 1, and
// the clock period is lower bounded by performing 1 FADD (expect
// about 100MHz).  The logic to map from iflit to oflit is substantial
// and must perform at least lg(STRM_WIDTH) deep in dependent FADDs,
// but this logic without depedence across iterations is trivially
// pipelinable.

template <typename TInPipe, typename TSumPipe, typename TDataPipe>
void prefixSumA() {
  [[intel::initiation_interval(1)]] // insist II=1 schedule
  while (1) {
    Flit iflit = TInPipe::read(); // get this iteration's flit
    Element partialSum = 0;

    // need to unroll to initiate on a new flit each cycle
#pragma unroll
    for (int64_t i = 0; i < STRM_WIDTH; i++) {
      partialSum += iflit.element[i];
    }

    TSumPipe::write(partialSum); // forward sum of this flit
    TDataPipe::write(iflit);     // forward flit
  }
}

template <typename TSumPipe, typename TDataPipe, typename TOutPipe>
void prefixSumB() {
  Element sumSoFar = 0; // this is state carried across time

  [[intel::initiation_interval(1)]] // insist II=1 schedule
  while (1) {
    // get this iteration's inputs (partial sum and flit)
    Flit iflit = TDataPipe::read();
    Element partialSum =
        TSumPipe::read(); // precomputed partial Sum of this flit

    Flit oflit;
    Element prefixSum = sumSoFar;

    // need to unroll to initiate on a new flit each cycle
#pragma unroll
    for (int64_t i = 0; i < STRM_WIDTH; i++) {
      prefixSum += iflit.element[i];
      oflit.element[i] = prefixSum;
    }

    sumSoFar += partialSum; // for next iterations
    TOutPipe::write(oflit); // forward flit
  }
}

///////////////////////////////////////////////////////////////////
//
// main(): this is just plumbing.
//
///////////////////////////////////////////////////////////////////
int main([[maybe_unused]] int argc, [[maybe_unused]] char **argv) {

  ////////////////////////////////////////////
  // Begin SYCL setup boiler plate
  ////////////////////////////////////////////
#if FPGA_SIMULATOR
  auto selector = sycl::ext::intel::fpga_simulator_selector_v;
#elif FPGA_HARDWARE
  auto selector = sycl::ext::intel::fpga_selector_v;
#elif FPGA_EMULATOR
  auto selector = sycl::ext::intel::fpga_emulator_selector_v;
#else
  auto selector = default_selector_v;
#endif

  double elapseTime = 0.0;

  // initialize SCYL device queue
  queue q(selector, exception_handler, property::queue::enable_profiling{});

  {
    // make sure the device supports USM device allocations
    auto device = q.get_device();

    if (!device.has(aspect::usm_device_allocations)) {
      std::cerr << "ERROR: The selected device does not support USM device"
                << " allocations\n";
      std::terminate();
    }

    std::cout << "Running on device: "
              << device.get_info<info::device::name>().c_str() << std::endl;
  }

  ////////////////////////////////////////////
  // End SYCL setup boiler plate
  ////////////////////////////////////////////

  ////////////////////////////////////////////
  // Set up test input and out buffers
  ////////////////////////////////////////////

#define STRM_LEN (1 << 24)

  // allocate and initialize buffers in host memory
  std::vector<Flit> idataBuf(2 * STRM_LEN / STRM_WIDTH);
  std::vector<Flit> odataBuf(2 * STRM_LEN / STRM_WIDTH);

  for (int64_t i = 0; i < STRM_LEN; i++) {
    idataBuf[i / STRM_WIDTH].element[i % STRM_WIDTH] = i;
    odataBuf[i / STRM_WIDTH].element[i % STRM_WIDTH] = 0x0;
  }

  // allocate device buffers
  sycl::buffer<Flit, 1> idataBuf_usm(idataBuf);
  sycl::buffer<Flit, 1> odataBuf_usm(odataBuf);

  ////////////////////////////////////////////
  // The fun stuff
  ////////////////////////////////////////////

  try {
    //
    // Launching kernels back-to-front because we are using the source
    // stage for timing.
    //

    // Launch a kernel that copies output data from a pipe
    // to USM memory for host to read
    event eventSink = q.submit([&](handler &h) {
      sycl::accessor obufPtr(odataBuf_usm, h, sycl::write_only);
      h.single_task<class Pipe2Output>([=]() {
        [[intel::initiation_interval(1)]] // insist II=1 schedule
        for (int64_t i = 0; i < STRM_LEN; i += STRM_WIDTH) {
          Flit flit = OutPipe::read();
          obufPtr[i / STRM_WIDTH] = flit;
        }
      });
    });

    // Launch processing kernels to compute prefix sum on the data
    // stream.  You can try two different ways to compute the prefix
    // sum.
#if 0
    q.submit([&](handler &h) {
      h.single_task<class PrefixSum>([=]() {
	prefixSumSimple<InPipe, OutPipe>();
      });
    });
#else
    q.submit([&](handler &h) {
      h.single_task<class PrefixSumB>(
          [=]() { prefixSumB<SumPipe, DataPipe, OutPipe>(); });
    });

    q.submit([&](handler &h) {
      h.single_task<class PrefixSumA>(
          [=]() { prefixSumA<InPipe, SumPipe, DataPipe>(); });
    });
#endif

    // Launch a kernel that copies input data from USM
    // memory (initialized by host) into a pipe
    event eventSource = q.submit([&](handler &h) {
      sycl::accessor ibufPtr(idataBuf_usm, h, sycl::read_only);
      h.single_task<class Input2Pipe>([=]() {
        [[intel::initiation_interval(1)]] // insist II=1 schedule
        for (int64_t i = 0; i < STRM_LEN; i += STRM_WIDTH) {
          Flit flit = ibufPtr[i / STRM_WIDTH];
          InPipe::write(flit);
        }
      });
    });

    eventSource.wait();
    eventSink.wait();
    elapseTime =
        eventSource
            .template get_profiling_info<info::event_profiling::command_end>() -
        eventSource.template get_profiling_info<
            info::event_profiling::command_start>();

  } catch (std::exception const &e) {
    std::cerr << "Exception! " << e.what() << std::endl;
    std::terminate();
  }

  // This will cause data to migrate from device to host
  sycl::host_accessor idataPtr(idataBuf_usm, read_only);
  sycl::host_accessor odataPtr(odataBuf_usm, read_only);

#if 0
  for (int i = 0; i < 16; i++) {
    // look at a few outputs
    std::cout << odataPtr[i / STRM_WIDTH].element[i % STRM_WIDTH] << std::endl;
  }
#endif
  
  {
    Element prefixSum = 0;
    for (int64_t i = 0; i < STRM_LEN; i += STRM_WIDTH) {
      for (int64_t j = 0; j < STRM_WIDTH; j++) {
        prefixSum += idataPtr[i / STRM_WIDTH].element[j];
        if (prefixSum != odataPtr[i / STRM_WIDTH].element[j]) {
          std::cout << "Sum incorrect." << std::endl;
          return 1;
        }
      }
    }
  }

  std::cout << "(Read->Pipe->Write Different Channel): "
            << elapseTime / 1E9 * 1E3 << " ms" << std::endl;
  std::cout << "Streaming BW: "
            << sizeof(Element) * (STRM_LEN / 1E9) / (elapseTime / 1E9)
            << " GB/sec" << std::endl;

  return 0;
}
