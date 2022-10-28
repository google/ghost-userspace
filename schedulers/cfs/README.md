# ghOSt CFS Agent

CFS is the default scheduler in the Linux kernel. The CFS agent is a (currently
incomplete) implementation of this scheduling policy as a ghost userspace
agent. Currently it assigns new tasks in a round-robin fashion to CPUs. Each CPU
has a runqueue; when ghost receives a message to schedule a ghost task on a cpu,
it simply plucks the one with the lowest vruntime.

To bring this agent to parity with CFS in the kernel, some items left to
implement are:

-   load balancing

-   nice values

-   work stealing

-   group scheduling

Once at feature parity, this agent can be used to deduce the "ghost" tax and
be used to quickly iterate on parameter tuning.
