// audio/audio_sink_null.cpp — NullSink factory.
//
// NullSink itself is defined inline in audio_sink.hpp so test binaries can
// construct it without linking any audio backend (the M2 debt fix: AudioSink
// is injected, so engine_test uses NullSink and never touches miniaudio). This
// TU provides the shared factory used by app/engine wiring and by tests that
// want the sink behind the AudioSink interface.
#include "audio/audio_sink.hpp"

namespace bootamp::audio {

std::shared_ptr<AudioSink> make_null_sink() {
  return std::make_shared<NullSink>();
}

}  // namespace bootamp::audio
