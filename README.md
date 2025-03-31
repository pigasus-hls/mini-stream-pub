# mini-stream
A small streaming SYCL example to help you get started.  This is done in Intel SYCL/DPC++, but AMD Vitis is analogous and just as easy.

## Describing Simple Pipelined Streaming Kernels ##

A basic SYCL streaming stage kernel follows the following template.

```c
kernel() {
    "architectural state" variables;

    while(1) {
        "transient state" variables;

        read input stream pipe;  // could read multiple pipes
 
        loop body (can do anything but must statically elaborate into a dataflow)

        write output stream pipe; // could write multiple pipes
    }
}
```

It doesn’t matter how complicated the loop body is, it should be read as an atomic transition that maps from the architectural state value from the current interation to the next iteration.  In many cases, the SYCL compiler can do a great job in correctly pipelining the loop to hit the desired clock frequency target and initiating 1 iteration per cycle.  

It would seem obvious that the design implied by the loopbody must be pipelinable in the first place for SYCL HLS to be able to generate a good pipeline.  What this means in practice is that you need be conscientous of architectural state variables' read-after-write dependence across successive iterations.  If you insist on an II=1 schedule, the longest of such a path (to produce write value after reading in same iteration) determines the clock frequency.  If you set a frequency target and let the compiler pick II, the longest of such a path determines II.

## Describing Pipelined Streaming Kernels Explicitly ##

More difficult cases will require the desired pipelining to be made explicit in the code.  (This doesn't happen in the MSPM example except in the compacter. The compacter is a rather extreme example in being structurally and timing-wise explicit.) 

For a basic 2-stage example without feedback path between stages, you could write:

```c
kernel() {
    stage1 "architectural state" variables;
    stage2 "architectural state" variables;
    pipeline stage register variables // feed forward from stage 1 to 2

    while(1) {

          // stage 2
          {
              "transient state" variables;
              stage 2 code that can read (but not write) pipeline stage register variables and read/write stage 2 architectural state variables.
              write output stream pipe;
          }
          // stage 1
          {
              "transient state" variables;
              read input stream pipe;
              stage 1 code that can read/write stage 1 architectural state variables and write (but not read) pipline stage register variables
          }
          // note stages 1 and 2 ordered so there is no RAW on the pipeline register variables within an iteration.
     }
}
```
You can express just about any pipeline you could write in RTL. (A general approach is described in https://arxiv.org/pdf/1710.10290).  Keep in mind, by making pipelining explicit this way, you are restricting what the compiler can do to pipeline further for timing.  You can also watch this talk https://www.youtube.com/watch?v=HDAEMMhOe60&feature=youtu.be for more ideas.

## Fast-path Slow-path ##

A good hardware design practice is to optimize for the common (often simpler) case.  Deprioritizing rare cases makes the common case easier to handle and faster.  

```c
kernel() {
    "architectural state" variables;

    while(1) {
        bool okay=true;
        do {
            // efficiently pipelined fast path
            "transient state" variables;

            read input stream pipe;
 
            loop body (can do anything but must statically elaborate into a dataflow);
            the loop body should also determine "okay"

            if (okay) write output stream pipe;
        } while (okay);

       // slow path for stage 2
        any special handling; you can use most of C and let the compiler do what it wants.
        write output stream pipe;
    }
}
```

If *okay* can be determined from the pipe input with shallow logic (1 cyc), the fast path loop will pipeline with II=1 straightforwardly.  If *okay* cannot be determined within 1 cycle, the code above cannot be pipelined with II=1 because the compiler cannot speculate ahead to read the input pipe for the next cycle until *okay* is decided for the current iteration.   In this case, you have to make the pipelining and intermedaite state explicit.  In the explicit version, the input pipe will be read speculatively for the next iteration, and the result is held by the pipeline stage register until the pipeline resumes.  For example, if okay takes 2 cycles to decide, you could do the following:

```c
kernel() {
    stage1 "architectural state" variables;
    stage2 "architectural state" variables;
    pipeline stage register variables

    while(1) {
        bool okay=true;
          
        do {
            // efficiently pipelined fast path

            // stage 2
            {
                "transient state" variables;
                stage 2 code that can read (but not write) pipeline stage register variables and read/write stage 2 architectural state variables.
                state 2 determines "okay";
                if (okay) write output stream pipe;
            }
            // stage 1
            {
                "transient state" variables;
                read input stream pipe;
                stage 1 code that can read/write stage 1 architectural state variables and write (but not read) pipline stage register variables
            }              
        } while (okay);

        // slow path for stage 2
        any special handling; you can use most of C and let the compiler do what it wants.
        write output stream pipe;
    }
}
```

## A word of caution on dependence originating from a nonblocking pipe read ##

Below is a special variation on the basic pipeline above.  The example is contrived to setup a dependence from the value read from the input pipe to whether the input pipe is read in the next iteration. It important to note that a nonblock version of pipe read is used so it is legal for the input pipe, on any given cycle for whatever reason, to say there is nothing to read even when there really is.  

```c
kernel() {
    "architectural state" variables;
    bool okay=true;

    while(1) {
        "transient state" variables;

        if (okay) nonblocking read input stream pipe;
        // "valid" is set if successful

        // loop body will consider "valid" and set "okay"
        loop body (can do anything but must statically elaborate into a dataflow)

        if (!okay) write output stream pipe;
   }
}
```

You would expect the depth of this dependence path to affect II and frequency, but something unexpected happens. If you set a frequency target such that the dependence path requires multiple cycles, the compiler will still report II=1 under a special interpretation where the pipe-read would internally wait long enough in between valid outputs. The symptom is that (1) even though II=1 is reported and (2) there is always new input pending, you observe emperically the pipeline does not start on a new input on every cycle possible. (The pipeline however does start a new iteration every cycle, hence II=1 is not false.)



