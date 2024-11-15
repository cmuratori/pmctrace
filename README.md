# pmctrace

Performance Monitoring Counter collection via Event Tracing for Windows – from [The Computer Enhance 2024 International Event Tracing for Windows Halloween Spooktacular Challenge](https://www.computerenhance.com/p/announcing-the-etw-halloween-spooktacular).

After much blood, sweat, and tears – both mine and Mārtiņš Možeiko's (see [Special Thanks](#special-thanks)) – pmctrace is a complete working demonstration of live _region-based_ PMC collection on Windows _without_ any third-party driver requirements.

Based on the ETW API documentation alone, one would think it impossible to have "rdpmc"-style access to PMCs on Windows. However, if you combine custom events with some careful abuse of DPC tracing, it turns out you _can_ make it work. It is still way more code – and way more overhead – than would have been necessary were the ETW API sane, but, at least we have now proven it's _possible_ to have PMC collection markup like this:

```
    pmc_traced_region Region;

    StartCountingPMCs(Tracer, &Region);
    // ... any code you want to measure goes here ...
    StopCountingPMCs(Tracer, &Region);
```

provide accurate, real-time PMC measurements on a vanilla install of Windows – no third-party kernel drivers required. It works properly with multiple regions, across multiple threads, and returns results directly to the program while it's running.

# Limitations

Unfortunately, several unavoidable limitations of the ETW API persist despite our best efforts. Specifically:

* This method requires SysCall event collection, which introduces unnecessary overhead when profiling code that performs a lot of actual system calls. This is not an issue for microbenchmarking runs, but could be prohibitive for inline profiling of full applications.
* Unlike rdpmc, results have some latency, so users who don't want to stall must write their code to poll rather than block

# Special Thanks

I would once again like to thank [Mārtiņš Možeiko](https://gist.github.com/mmozeiko) for fearlessly producing great reference material for some of the world’s most finicky tools and APIs. The startup code for pmctrace was modeled after [Mārtiņš' epic miniperf gist](https://gist.github.com/mmozeiko/bd5923bcd9d20b5b9946691932ec95fa). ETW is incredibly flakey, and often fails to work for undocumented or convoluted system configuration reasons. Having miniperf to work from and check against was of the few things that allowed me to stay (mostly) sane in the process.
