find_package(Threads REQUIRED)

xdec_add_library(xdec_obs
  SOURCES
    clock.cpp
    context.cpp
    field.cpp
    job.cpp
    log.cpp
    metrics.cpp
    monitor.cpp
    process.cpp
    recorder.cpp
    sinks.cpp
    span.cpp
    threads.cpp
  DEPS
    Threads::Threads)
