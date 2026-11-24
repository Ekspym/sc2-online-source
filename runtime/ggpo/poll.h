#ifndef _POLL_H
#define _POLL_H
#include "static_buffer.h"

class IPollSink {
public:
   virtual ~IPollSink() { }
   virtual bool OnLoopPoll(void *) { return true; }
};

class Poll {
public:
   void RegisterLoop(IPollSink *sink, void *cookie = NULL) { _loop_sinks.push_back(PollSinkCb(sink, cookie)); }
   bool Pump(int timeout);

protected:
   struct PollSinkCb {
      IPollSink   *sink;
      void        *cookie;
      PollSinkCb() : sink(NULL), cookie(NULL) { }
      PollSinkCb(IPollSink *s, void *c) : sink(s), cookie(c) { }
   };
   StaticBuffer<PollSinkCb, 16>          _loop_sinks;
};
#endif
