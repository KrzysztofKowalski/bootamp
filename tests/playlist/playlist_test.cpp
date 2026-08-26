// tests/playlist/playlist_test.cpp — Catch2 port of cliamp/playlist tests.
//
// Ports playlist_test.go, navigation_test.go, queue_test.go, revision_test.go,
// shuffle_repeat_test.go, ownership_test.go, url_test.go and track_test.go.
// Tests that belong to the tags module (FileURL, cacheAlbumArt,
// RefreshEmbeddedMetadata, CleanupAlbumArtCache) and the encoding module's
// direct sanitizeTag calls are not ported (not part of this module's contract;
// sanitizeTag is internal to playlist.cpp).
#include <catch2/catch_test_macros.hpp>

#include "playlist/playlist.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using bootamp::playlist::Playlist;
using bootamp::playlist::RepeatMode;
using bootamp::playlist::SelectionActivation;
using bootamp::playlist::Snapshot;
using bootamp::playlist::Track;

namespace {

// Go makePlaylist: n tracks titled "A", "B", "C", ... (shuffle toggled before
// Replace so Replace triggers the initial shuffle). Playlist is non-movable
// (mutex + atomic), so we return a unique_ptr.
std::unique_ptr<Playlist> make_playlist(int n, bool shuffle) {
  std::vector<Track> tracks;
  tracks.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    Track t;
    t.title = std::string(1, static_cast<char>('A' + i));
    tracks.push_back(t);
  }
  auto p = std::make_unique<Playlist>();
  if (shuffle) {
    p->toggle_shuffle();
  }
  p->replace(tracks);
  return p;
}

std::vector<std::string> titles(const Playlist& p) {
  std::vector<std::string> out;
  for (const Track& t : p.tracks()) {
    out.push_back(t.title);
  }
  return out;
}

bool slice_eq(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  return a == b;
}

// Go assertQueuePositions: QueuePosition(idx) must match `want` for every idx.
void assert_queue_positions(Playlist& p, const std::map<int, int>& want) {
  const int n = p.len();
  for (int idx = 0; idx < n; ++idx) {
    const auto it = want.find(idx);
    const int expected = it == want.end() ? 0 : it->second;
    REQUIRE(p.queue_position(idx) == expected);
  }
}

}  // namespace

// ============================================================================
// playlist_test.go
// ============================================================================
TEST_CASE("MoveDown", "[playlist]") {
  auto p = make_playlist(5, false);  // A B C D E
  p->set_index(0);                        // playing A

  REQUIRE(p->move(1, 2));

  REQUIRE(slice_eq(titles(*p), {"A", "C", "B", "D", "E"}));

  auto [cur, idx] = p->current();
  REQUIRE(cur.title == "A");
  REQUIRE(idx == 0);
}

TEST_CASE("MoveUp", "[playlist]") {
  auto p = make_playlist(5, false);  // A B C D E
  p->set_index(3);                        // playing D

  REQUIRE(p->move(3, 2));

  REQUIRE(slice_eq(titles(*p), {"A", "B", "D", "C", "E"}));

  auto [cur, idx] = p->current();
  REQUIRE(cur.title == "D");
  REQUIRE(idx == 2);
}

TEST_CASE("MoveCurrentTrack", "[playlist]") {
  auto p = make_playlist(4, false);  // A B C D
  p->set_index(1);                        // playing B

  REQUIRE(p->move(1, 2));

  REQUIRE(slice_eq(titles(*p), {"A", "C", "B", "D"}));

  auto [cur, idx] = p->current();
  REQUIRE(cur.title == "B");
  REQUIRE(idx == 2);
}

TEST_CASE("MoveBoundary", "[playlist]") {
  auto p = make_playlist(3, false);

  REQUIRE_FALSE(p->move(0, -1));
  REQUIRE_FALSE(p->move(2, 3));
  REQUIRE_FALSE(p->move(1, 1));
}

TEST_CASE("MovePreservesPlaybackOrderNoShuffle", "[playlist]") {
  auto p = make_playlist(5, false);  // A B C D E
  p->set_index(0);

  p->move(2, 1);  // A C B D E

  std::vector<std::string> playback;
  auto [cur, _] = p->current();
  playback.push_back(cur.title);  // A

  for (int i = 0; i < 4; ++i) {
    auto [track, ok] = p->next();
    REQUIRE(ok);
    playback.push_back(track.title);
  }

  REQUIRE(playback == std::vector<std::string>({"A", "C", "B", "D", "E"}));
}

TEST_CASE("MoveWithQueue", "[playlist]") {
  auto p = make_playlist(4, false);  // A B C D
  p->queue(2);                            // queue C (index 2)

  p->move(2, 1);  // A C B D, queue should now reference index 1

  REQUIRE(p->queue_position(1) == 1);
  REQUIRE(p->queue_position(2) == 0);
}

TEST_CASE("MoveShuffle", "[playlist]") {
  auto p = make_playlist(5, true);  // shuffled

  Snapshot before = p->snapshot();
  p->set_index(before.order[0]);
  const std::vector<int> order_before = before.order;
  const std::vector<Track> tracks_before = before.tracks;
  const std::string t0 = tracks_before[0].title;
  const std::string t1 = tracks_before[1].title;

  p->move(1, 0);

  const std::vector<Track> after = p->tracks();
  REQUIRE(after[0].title == t1);
  REQUIRE(after[1].title == t0);

  // The shuffle order should still reference the same tracks.
  const std::vector<int> order_after = p->snapshot().order;
  for (size_t i = 0; i < order_after.size(); ++i) {
    const std::string got = after[static_cast<size_t>(order_after[i])].title;
    std::string want;
    if (order_before[i] == 0) {
      want = t0;
    } else if (order_before[i] == 1) {
      want = t1;
    } else {
      want = tracks_before[static_cast<size_t>(order_before[i])].title;
    }
    REQUIRE(got == want);
  }
}

TEST_CASE("AddShufflesNewTracksWhenShuffleEnabled", "[playlist]") {
  auto p = make_playlist(10, true);
  Snapshot s0 = p->snapshot();
  p->set_index(s0.order[0]);  // ensure pos is valid and stable
  auto [cur, cur_idx] = p->current();

  const int start = p->len();
  std::vector<Track> added;
  for (int i = 0; i < 30; ++i) {
    Track t;
    t.title = std::string(1, static_cast<char>('K' + i));
    added.push_back(t);
  }
  p->add(added);

  auto [cur2, cur_idx2] = p->current();
  REQUIRE(cur2.title == cur.title);
  REQUIRE(cur_idx2 == cur_idx);

  // Added tracks must be interleaved with existing upcoming tracks, not just
  // shuffled among themselves at the tail.
  const Snapshot s1 = p->snapshot();
  const std::vector<int> upcoming(s1.order.begin() + s1.pos + 1,
                                  s1.order.end());
  auto is_new = [&](int idx) { return idx >= start; };
  int last_new = -1;
  bool found_old_after_new = false;
  for (size_t i = 0; i < upcoming.size(); ++i) {
    if (is_new(upcoming[i])) {
      last_new = static_cast<int>(i);
    } else if (last_new >= 0) {
      found_old_after_new = true;
      break;
    }
  }
  REQUIRE(last_new >= 0);
  if (!found_old_after_new && last_new < static_cast<int>(upcoming.size()) - 1) {
    FAIL("added tracks are not interleaved with existing tracks");
  }
}

TEST_CASE("AddDoesNotShuffleWhenShuffleDisabled", "[playlist]") {
  auto p = make_playlist(5, false);
  p->set_index(2);
  auto [cur, cur_idx] = p->current();

  p->add({Track{.title = "F"}, Track{.title = "G"}});

  auto [cur2, cur_idx2] = p->current();
  REQUIRE(cur2.title == cur.title);
  REQUIRE(cur_idx2 == cur_idx);

  const std::vector<int> want_order = {0, 1, 2, 3, 4, 5, 6};
  REQUIRE(p->snapshot().order == want_order);
}

TEST_CASE("MoveQueue", "[playlist]") {
  auto p = make_playlist(5, false);  // A B C D E
  p->queue(3);                            // D
  p->queue(1);                            // B
  p->queue(4);                            // E
  // Queue order: D, B, E

  REQUIRE(p->move_queue(1, 0));

  auto qt = p->queue_tracks();
  REQUIRE(qt.size() == 3);
  REQUIRE(qt[0].title == "B");
  REQUIRE(qt[1].title == "D");
  REQUIRE(qt[2].title == "E");

  REQUIRE(p->move_queue(0, 1));

  qt = p->queue_tracks();
  REQUIRE(qt[0].title == "D");
  REQUIRE(qt[1].title == "B");
  REQUIRE(qt[2].title == "E");
}

TEST_CASE("MoveQueueBoundary", "[playlist]") {
  auto p = make_playlist(3, false);
  p->queue(0);
  p->queue(1);

  REQUIRE_FALSE(p->move_queue(0, -1));
  REQUIRE_FALSE(p->move_queue(1, 2));
  REQUIRE_FALSE(p->move_queue(0, 0));
}

TEST_CASE("RemoveShiftsHigherIndices", "[playlist]") {
  auto p = make_playlist(5, false);  // A B C D E
  p->set_index(3);                        // playing D
  p->queue(4);                            // queue E

  REQUIRE(p->remove(1));  // remove B

  REQUIRE(slice_eq(titles(*p), {"A", "C", "D", "E"}));

  auto [cur, idx] = p->current();
  REQUIRE(cur.title == "D");
  REQUIRE(idx == 2);

  auto qt = p->queue_tracks();
  REQUIRE(qt.size() == 1);
  REQUIRE(qt[0].title == "E");
}

TEST_CASE("RemoveCurrentTrack", "[playlist]") {
  auto p = make_playlist(4, false);  // A B C D
  p->set_index(1);                        // playing B

  REQUIRE(p->remove(1));

  REQUIRE(slice_eq(titles(*p), {"A", "C", "D"}));

  auto [cur, idx] = p->current();
  REQUIRE(cur.title == "C");
  REQUIRE(idx == 1);
}

TEST_CASE("RemoveLastTrack", "[playlist]") {
  auto p = make_playlist(2, false);  // A B
  p->set_index(1);                        // playing B

  REQUIRE(p->remove(1));

  REQUIRE(p->len() == 1);
  auto [cur, idx] = p->current();
  REQUIRE(cur.title == "A");
  REQUIRE(idx == 0);
}

TEST_CASE("RemoveOutOfBounds", "[playlist]") {
  auto p = make_playlist(3, false);
  REQUIRE_FALSE(p->remove(-1));
  REQUIRE_FALSE(p->remove(3));
  REQUIRE(p->len() == 3);
}

TEST_CASE("NextPreservesCurrentOnUnplayableTail", "[playlist]") {
  Playlist p;
  p.replace({Track{.title = "A"},
             Track{.title = "B", .unplayable = true},
             Track{.title = "C", .unplayable = true}});
  p.set_index(0);

  REQUIRE_FALSE(p.next().second);

  auto [track, idx] = p.current();
  REQUIRE(track.title == "A");
  REQUIRE(idx == 0);
}

TEST_CASE("PrevPreservesCurrentOnUnplayableHead", "[playlist]") {
  Playlist p;
  p.replace({Track{.title = "A", .unplayable = true},
             Track{.title = "B", .unplayable = true},
             Track{.title = "C"}});
  p.set_index(2);

  REQUIRE_FALSE(p.prev().second);

  auto [track, idx] = p.current();
  REQUIRE(track.title == "C");
  REQUIRE(idx == 2);
}

TEST_CASE("PeekNextMatchesNext", "[playlist]") {
  Playlist p;
  p.replace({Track{.title = "A"},
             Track{.title = "B", .unplayable = true},
             Track{.title = "C"}});
  p.set_index(0);
  p.queue(1);
  p.queue(2);

  auto [peek, ok] = p.peek_next();
  REQUIRE(ok);
  REQUIRE(peek.title == "C");
  auto [cur, idx] = p.current();
  REQUIRE(cur.title == "A");
  REQUIRE(idx == 0);
  REQUIRE(p.queue_len() == 2);

  auto [next, ok2] = p.next();
  REQUIRE(ok2);
  REQUIRE(next.title == peek.title);
}

TEST_CASE("NextConsumesUnplayableQueuedItemsOnFailure", "[playlist]") {
  Playlist p;
  p.replace({Track{.title = "A"},
             Track{.title = "B", .unplayable = true}});
  p.set_index(0);
  p.queue(1);

  REQUIRE_FALSE(p.next().second);

  auto [cur, idx] = p.current();
  REQUIRE(cur.title == "A");
  REQUIRE(idx == 0);
  REQUIRE(p.queue_len() == 0);
}

TEST_CASE("NextFailurePreservesQueuedCurrentTrack", "[playlist]") {
  Playlist p;
  p.replace({Track{.title = "A"}, Track{.title = "B"}});
  p.set_index(1);
  p.queue(0);

  auto [track, ok] = p.next();
  REQUIRE(ok);
  REQUIRE(track.title == "A");

  REQUIRE_FALSE(p.next().second);

  auto [cur, idx] = p.current();
  REQUIRE(cur.title == "A");
  REQUIRE(idx == 0);
}

TEST_CASE("NextRepeatAllShuffleWrapSkipsCurrentTrack", "[playlist]") {
  Playlist p;
  Snapshot s;
  s.tracks = {Track{.title = "A"},
              Track{.title = "B"},
              Track{.title = "C", .unplayable = true}};
  s.order = {1, 2, 0};
  s.pos = 2;
  s.shuffle = true;
  s.repeat = RepeatMode::All;
  p.restore(s);

  auto [track, ok] = p.next();
  REQUIRE(ok);
  REQUIRE(track.title == "B");
  auto [_, idx] = p.current();
  REQUIRE(idx == 1);
}

TEST_CASE("NextRepeatAllShuffleWrapFailureKeepsCurrentTrack", "[playlist]") {
  Playlist p;
  Snapshot s;
  s.tracks = {Track{.title = "A", .unplayable = true},
              Track{.title = "B", .unplayable = true},
              Track{.title = "C", .unplayable = true}};
  s.order = {0, 1, 2};
  s.pos = 2;
  s.shuffle = true;
  s.repeat = RepeatMode::All;
  p.restore(s);

  auto [before, before_idx] = p.current();

  REQUIRE_FALSE(p.next().second);

  auto [after, after_idx] = p.current();
  REQUIRE(after.title == before.title);
  REQUIRE(after_idx == before_idx);
}

TEST_CASE("NextRepeatOneUnplayableCurrentReturnsFalse", "[playlist]") {
  Playlist p;
  p.set_repeat(RepeatMode::One);
  p.replace({Track{.title = "A", .unplayable = true},
             Track{.title = "B"},
             Track{.title = "C"}});
  p.set_index(0);

  REQUIRE_FALSE(p.next().second);

  auto [track, idx] = p.current();
  REQUIRE(track.title == "A");
  REQUIRE(idx == 0);
}

TEST_CASE("ActivateSelected", "[playlist]") {
  SECTION("pending queued track") {
    Playlist p;
    p.replace({Track{.title = "Queued"},
               Track{.title = "Missing", .unplayable = true},
               Track{.title = "Replacement"}});
    p.set_index(1);
    p.queue(0);

    std::optional<SelectionActivation> activation = p.activate_selected();
    REQUIRE(activation.has_value());
    REQUIRE(activation->track.title == "Replacement");
    REQUIRE(activation->index == 2);
    REQUIRE(activation->skipped);

    auto [current, idx] = p.current();
    REQUIRE(current.title == "Replacement");
    REQUIRE(idx == 2);
    REQUIRE(p.queue_len() == 1);
    REQUIRE(p.queue_position(0) == 1);
  }

  SECTION("active queued current") {
    Playlist p;
    p.replace({Track{.title = "Queued"},
               Track{.title = "Missing", .unplayable = true},
               Track{.title = "Replacement"}});
    p.set_index(1);
    p.queue(0);
    auto [track, ok] = p.next();
    REQUIRE(ok);
    REQUIRE(track.title == "Queued");

    std::optional<SelectionActivation> activation = p.activate_selected();
    REQUIRE(activation.has_value());
    REQUIRE(activation->track.title == "Replacement");
    REQUIRE(activation->index == 2);
    REQUIRE(activation->skipped);

    auto [current, idx] = p.current();
    REQUIRE(current.title == "Replacement");
    REQUIRE(idx == 2);
    REQUIRE(p.queue_len() == 0);
  }

  SECTION("wraps with repeat all") {
    Playlist p;
    p.set_repeat(RepeatMode::All);
    p.replace({Track{.title = "A"},
               Track{.title = "B"},
               Track{.title = "C", .unplayable = true}});
    p.set_index(2);

    std::optional<SelectionActivation> activation = p.activate_selected();
    REQUIRE(activation.has_value());
    REQUIRE(activation->track.title == "A");
    REQUIRE(activation->index == 0);
    REQUIRE(activation->skipped);
  }

  SECTION("failure keeps queued current track") {
    Playlist p;
    p.replace({Track{.title = "Queued"},
               Track{.title = "Missing", .unplayable = true},
               Track{.title = "Still Missing", .unplayable = true}});
    p.set_index(1);
    p.queue(0);
    p.queue(2);
    auto [track, ok] = p.next();
    REQUIRE(ok);
    REQUIRE(track.title == "Queued");

    REQUIRE_FALSE(p.activate_selected().has_value());

    auto [current, idx] = p.current();
    REQUIRE(current.title == "Queued");
    REQUIRE(idx == 0);
    REQUIRE(p.queue_len() == 1);
    REQUIRE(p.queue_position(2) == 1);
  }
}

TEST_CASE("TotalDurationSecs", "[playlist]") {
  const std::vector<Track> tracks = {Track{.duration_secs = 100},
                                     Track{.duration_secs = 0},
                                     Track{.duration_secs = 200}};
  REQUIRE(bootamp::playlist::total_duration_secs(tracks) == 300);
  REQUIRE(bootamp::playlist::total_duration_secs({}) == 0);
}

// ============================================================================
// navigation_test.go
// ============================================================================
TEST_CASE("NextRepeatOff", "[playlist]") {
  auto p = make_playlist(3, false);  // A B C

  auto [track, ok] = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "B");

  std::tie(track, ok) = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "C");

  REQUIRE_FALSE(p->next().second);
}

TEST_CASE("NextRepeatAll", "[playlist]") {
  auto p = make_playlist(3, false);  // A B C
  p->set_repeat(RepeatMode::All);

  p->next();  // B
  p->next();  // C

  auto [track, ok] = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "A");
}

TEST_CASE("NextRepeatOne", "[playlist]") {
  auto p = make_playlist(3, false);  // A B C
  p->set_repeat(RepeatMode::One);

  auto [track, ok] = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "A");

  std::tie(track, ok) = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "A");
}

TEST_CASE("NextWithQueue", "[playlist]") {
  auto p = make_playlist(4, false);  // A B C D
  p->queue(2);                            // queue C
  p->queue(3);                            // queue D

  auto [track, ok] = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "C");

  std::tie(track, ok) = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "D");

  std::tie(track, ok) = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "B");
}

TEST_CASE("NextRepeatOneResumesOrderAfterQueuedTrack", "[playlist]") {
  auto p = make_playlist(2, false);  // A B
  p->set_repeat(RepeatMode::One);
  p->queue(1);

  auto [track, ok] = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "B");

  std::tie(track, ok) = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "A");

  auto [current, idx] = p->current();
  REQUIRE(current.title == "A");
  REQUIRE(idx == 0);
}

TEST_CASE("PrevBasic", "[playlist]") {
  auto p = make_playlist(3, false);
  p->set_index(2);  // C

  auto [track, ok] = p->prev();
  REQUIRE(ok);
  REQUIRE(track.title == "B");

  std::tie(track, ok) = p->prev();
  REQUIRE(ok);
  REQUIRE(track.title == "A");

  REQUIRE_FALSE(p->prev().second);
}

TEST_CASE("PrevRepeatAll", "[playlist]") {
  auto p = make_playlist(3, false);
  p->set_repeat(RepeatMode::All);

  auto [track, ok] = p->prev();
  REQUIRE(ok);
  REQUIRE(track.title == "C");
}

TEST_CASE("PeekNextBasic", "[playlist]") {
  auto p = make_playlist(3, false);

  auto [track, ok] = p->peek_next();
  REQUIRE(ok);
  REQUIRE(track.title == "B");

  auto [cur, _] = p->current();
  REQUIRE(cur.title == "A");
}

TEST_CASE("PeekNextAtEnd", "[playlist]") {
  auto p = make_playlist(3, false);
  p->set_index(2);  // C

  REQUIRE_FALSE(p->peek_next().second);
}

TEST_CASE("PeekNextRepeatAll", "[playlist]") {
  auto p = make_playlist(3, false);
  p->set_repeat(RepeatMode::All);
  p->set_index(2);  // C

  auto [track, ok] = p->peek_next();
  REQUIRE(ok);
  REQUIRE(track.title == "A");
}

TEST_CASE("PeekNextRepeatOne", "[playlist]") {
  auto p = make_playlist(3, false);
  p->set_repeat(RepeatMode::One);
  p->set_index(1);  // B

  auto [track, ok] = p->peek_next();
  REQUIRE(ok);
  REQUIRE(track.title == "B");
}

TEST_CASE("PeekNextWithQueue", "[playlist]") {
  auto p = make_playlist(3, false);
  p->queue(2);  // queue C

  auto [track, ok] = p->peek_next();
  REQUIRE(ok);
  REQUIRE(track.title == "C");
}

TEST_CASE("PeekNextRepeatOneUsesOrderAfterQueuedTrack", "[playlist]") {
  auto p = make_playlist(2, false);  // A B
  p->set_repeat(RepeatMode::One);
  p->queue(1);

  auto [track, ok] = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "B");

  std::tie(track, ok) = p->peek_next();
  REQUIRE(ok);
  REQUIRE(track.title == "A");
}

TEST_CASE("EmptyPlaylist", "[playlist]") {
  Playlist p;

  auto [_, idx] = p.current();
  REQUIRE(idx == -1);

  REQUIRE(p.index() == -1);
  REQUIRE_FALSE(p.next().second);
  REQUIRE_FALSE(p.prev().second);
}

TEST_CASE("SetIndex", "[playlist]") {
  auto p = make_playlist(5, false);
  p->set_index(3);  // D

  auto [cur, idx] = p->current();
  REQUIRE(idx == 3);
  REQUIRE(cur.title == "D");
}

TEST_CASE("Replace", "[playlist]") {
  auto p = make_playlist(3, false);
  p->set_index(2);

  p->replace({Track{.title = "X"}, Track{.title = "Y"}});

  REQUIRE(p->len() == 2);
  auto [cur, idx] = p->current();
  REQUIRE(idx == 0);
  REQUIRE(cur.title == "X");
}

// ============================================================================
// queue_test.go
// ============================================================================
TEST_CASE("QueueAndDequeue", "[playlist]") {
  auto p = make_playlist(5, false);

  p->queue(2);
  p->queue(4);

  REQUIRE(p->queue_len() == 2);

  REQUIRE(p->dequeue(2));
  REQUIRE(p->queue_len() == 1);

  REQUIRE_FALSE(p->dequeue(2));
}

TEST_CASE("QueuePosition", "[playlist]") {
  auto p = make_playlist(5, false);

  p->queue(1);
  p->queue(3);
  p->queue(4);

  REQUIRE(p->queue_position(1) == 1);
  REQUIRE(p->queue_position(3) == 2);
  REQUIRE(p->queue_position(4) == 3);
  REQUIRE(p->queue_position(0) == 0);
}

TEST_CASE("QueueTracks", "[playlist]") {
  auto p = make_playlist(5, false);

  p->queue(0);  // A
  p->queue(2);  // C
  p->queue(4);  // E

  auto qt = p->queue_tracks();
  REQUIRE(qt.size() == 3);
  REQUIRE(qt[0].title == "A");
  REQUIRE(qt[1].title == "C");
  REQUIRE(qt[2].title == "E");
}

TEST_CASE("QueueWindow", "[playlist]") {
  auto p = make_playlist(5, false);
  for (int i = 0; i < 5; ++i) {
    p->queue(i);
  }

  SECTION("middle") {
    auto w = p->queue_window(1, 2);
    REQUIRE(w[0].title == "B");
    REQUIRE(w[1].title == "C");
  }
  SECTION("negative start") {
    auto w = p->queue_window(-2, 2);
    REQUIRE(w[0].title == "A");
    REQUIRE(w[1].title == "B");
  }
  SECTION("limit past end") {
    auto w = p->queue_window(3, 10);
    REQUIRE(w[0].title == "D");
    REQUIRE(w[1].title == "E");
  }
  SECTION("start at end") {
    REQUIRE(p->queue_window(5, 1).empty());
  }
  SECTION("zero limit") {
    REQUIRE(p->queue_window(0, 0).empty());
  }
  SECTION("negative limit") {
    REQUIRE(p->queue_window(0, -1).empty());
  }
  SECTION("mutation isolation") {
    auto window = p->queue_window(1, 1);
    window[0].title = "changed";
    REQUIRE(p->queue_window(1, 1)[0].title == "B");
  }
}

TEST_CASE("QueueOutputsOwnProviderMeta", "[playlist]") {
  Playlist p;
  p.replace({Track{.title = "", .provider_meta = {{"provider.id", "original"}}}});
  p.queue(0);

  auto window = p.queue_window(0, 1);
  window[0].provider_meta["provider.id"] = "window";
  auto queued = p.queue_tracks();
  queued[0].provider_meta["provider.id"] = "queue";

  REQUIRE(p.tracks()[0].provider_meta["provider.id"] == "original");
}

TEST_CASE("QueuePositionsStayCoherentAcrossMutations", "[playlist]") {
  auto p = make_playlist(5, false);
  p->queue(1);
  p->queue(3);
  p->queue(1);
  p->queue(4);
  assert_queue_positions(*p, {{1, 1}, {3, 2}, {4, 4}});

  REQUIRE(p->move_queue(3, 0));
  assert_queue_positions(*p, {{1, 3}, {3, 2}, {4, 1}});

  p->remove_queue_at(1);
  assert_queue_positions(*p, {{1, 2}, {4, 1}});

  REQUIRE(p->dequeue(1));
  assert_queue_positions(*p, {{1, 2}, {4, 1}});

  REQUIRE(p->move(1, 2));
  assert_queue_positions(*p, {{2, 2}, {4, 1}});
  Snapshot snapshot = p->snapshot();

  REQUIRE(p->remove(0));
  assert_queue_positions(*p, {{1, 2}, {3, 1}});

  p->clear_queue();
  assert_queue_positions(*p, {});
  p->restore(snapshot);
  assert_queue_positions(*p, {{2, 2}, {4, 1}});

  auto [track, ok] = p->next();
  REQUIRE(ok);
  REQUIRE(track.title == "E");
  assert_queue_positions(*p, {{2, 1}});

  p->replace({Track{.title = "replacement"}});
  assert_queue_positions(*p, {});
}

TEST_CASE("RestoreSnapshotRestoresTracksAndQueue", "[playlist]") {
  auto p = make_playlist(3, false);
  p->queue(0);
  p->queue(2);
  Snapshot snapshot = p->snapshot();

  p->remove(0);
  p->clear_queue();
  p->restore(snapshot);

  auto tracks = p->tracks();
  REQUIRE(tracks.size() == 3);
  REQUIRE(tracks[0].title == "A");
  auto queued = p->queue_tracks();
  REQUIRE(queued.size() == 2);
  REQUIRE(queued[0].title == "A");
  REQUIRE(queued[1].title == "C");
}

TEST_CASE("ClearQueue", "[playlist]") {
  auto p = make_playlist(3, false);
  p->queue(0);
  p->queue(1);

  p->clear_queue();

  REQUIRE(p->queue_len() == 0);
}

TEST_CASE("RemoveQueueAt", "[playlist]") {
  auto p = make_playlist(5, false);
  p->queue(0);  // A
  p->queue(2);  // C
  p->queue(4);  // E

  p->remove_queue_at(1);  // remove C

  auto qt = p->queue_tracks();
  REQUIRE(qt.size() == 2);
  REQUIRE(qt[0].title == "A");
  REQUIRE(qt[1].title == "E");
}

TEST_CASE("RemoveQueueAtBounds", "[playlist]") {
  auto p = make_playlist(3, false);
  p->queue(0);

  p->remove_queue_at(-1);
  p->remove_queue_at(5);

  REQUIRE(p->queue_len() == 1);
}

TEST_CASE("QueueBoundsCheck", "[playlist]") {
  auto p = make_playlist(3, false);

  p->queue(-1);
  p->queue(5);

  REQUIRE(p->queue_len() == 0);
}

// ============================================================================
// revision_test.go
// ============================================================================
TEST_CASE("RevisionIncrementsForStateChanges", "[playlist]") {
  struct Case {
    const char* name;
    // Go-style setup: configure `p`, return the mutation closure.
    std::function<std::function<void()>(Playlist&)> setup;
  };
  const std::vector<Case> cases = {
      {"add",
       [](Playlist& p) -> std::function<void()> {
         return [&p] { p.add({Track{.title = "A"}}); };
       }},
      {"replace",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}});
         return [&p] { p.replace({Track{.title = "B"}}); };
       }},
      {"activate selected queued track",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}, Track{.title = "B"}});
         p.queue(1);
         p.next();
         return [&p] { p.activate_selected(); };
       }},
      {"next",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}, Track{.title = "B"}});
         return [&p] { p.next(); };
       }},
      {"next clears unavailable queue",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}, Track{.title = "B", .unplayable = true}});
         p.queue(1);
         return [&p] { p.next(); };
       }},
      {"prev",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}, Track{.title = "B"}});
         p.set_index(1);
         return [&p] { p.prev(); };
       }},
      {"set index",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}, Track{.title = "B"}});
         return [&p] { p.set_index(1); };
       }},
      {"queue",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}});
         return [&p] { p.queue(0); };
       }},
      {"dequeue",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}});
         p.queue(0);
         return [&p] { p.dequeue(0); };
       }},
      {"remove queue entry",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}});
         p.queue(0);
         return [&p] { p.remove_queue_at(0); };
       }},
      {"move queue entry",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}, Track{.title = "B"}});
         p.queue(0);
         p.queue(1);
         return [&p] { p.move_queue(0, 1); };
       }},
      {"clear queue",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}});
         p.queue(0);
         return [&p] { p.clear_queue(); };
       }},
      {"move track",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}, Track{.title = "B"}});
         return [&p] { p.move(0, 1); };
       }},
      {"remove track",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}});
         return [&p] { p.remove(0); };
       }},
      {"set track",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}});
         return [&p] { p.set_track(0, Track{.title = "B"}); };
       }},
      {"toggle bookmark",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}});
         return [&p] { p.toggle_bookmark(0); };
       }},
      {"toggle shuffle",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}});
         return [&p] { p.toggle_shuffle(); };
       }},
      {"cycle repeat",
       [](Playlist& p) -> std::function<void()> {
         return [&p] { p.cycle_repeat(); };
       }},
      {"set repeat",
       [](Playlist& p) -> std::function<void()> {
         return [&p] { p.set_repeat(RepeatMode::All); };
       }},
      {"restore",
       [](Playlist& p) -> std::function<void()> {
         p.add({Track{.title = "A"}});
         const Snapshot snap = p.snapshot();
         p.add({Track{.title = "B"}});
         return [&p, snap] { p.restore(snap); };
       }},
  };

  for (const Case& c : cases) {
    SECTION(c.name) {
      Playlist p;
      auto mutate = c.setup(p);
      const std::uint64_t before = p.revision();
      mutate();
      REQUIRE(p.revision() == before + 1);
    }
  }
}

TEST_CASE("RevisionUnchangedByNoopMutations", "[playlist]") {
  struct Case {
    const char* name;
    std::function<void(Playlist&)> prep;
    std::function<void(Playlist&)> act;
  };
  const std::vector<Case> cases = {
      {"add no tracks",
       [](Playlist&) {},
       [](Playlist& p) { p.add({}); }},
      {"replace identical state",
       [](Playlist& p) {
         p.add({Track{.title = "A"}});
         (void)p.tracks();
       },
       [](Playlist& p) { p.replace(p.tracks()); }},
      {"activate current selection",
       [](Playlist& p) { p.add({Track{.title = "A"}}); },
       [](Playlist& p) { p.activate_selected(); }},
      {"next at end",
       [](Playlist& p) { p.add({Track{.title = "A"}}); },
       [](Playlist& p) { p.next(); }},
      {"next repeat one",
       [](Playlist& p) {
         p.add({Track{.title = "A"}});
         p.set_repeat(RepeatMode::One);
       },
       [](Playlist& p) { p.next(); }},
      {"prev at beginning",
       [](Playlist& p) { p.add({Track{.title = "A"}}); },
       [](Playlist& p) { p.prev(); }},
      {"prev repeat all one track",
       [](Playlist& p) {
         p.add({Track{.title = "A"}});
         p.set_repeat(RepeatMode::All);
       },
       [](Playlist& p) { p.prev(); }},
      {"set current index",
       [](Playlist& p) { p.add({Track{.title = "A"}}); },
       [](Playlist& p) { p.set_index(0); }},
      {"set invalid index",
       [](Playlist& p) { p.add({Track{.title = "A"}}); },
       [](Playlist& p) { p.set_index(1); }},
      {"queue invalid index",
       [](Playlist&) {},
       [](Playlist& p) { p.queue(0); }},
      {"dequeue missing track",
       [](Playlist& p) { p.add({Track{.title = "A"}}); },
       [](Playlist& p) { p.dequeue(0); }},
      {"remove invalid queue entry",
       [](Playlist&) {},
       [](Playlist& p) { p.remove_queue_at(0); }},
      {"move queue entry to itself",
       [](Playlist& p) {
         p.add({Track{.title = "A"}});
         p.queue(0);
       },
       [](Playlist& p) { p.move_queue(0, 0); }},
      {"clear empty queue",
       [](Playlist&) {},
       [](Playlist& p) { p.clear_queue(); }},
      {"move track to itself",
       [](Playlist& p) { p.add({Track{.title = "A"}}); },
       [](Playlist& p) { p.move(0, 0); }},
      {"remove invalid track",
       [](Playlist&) {},
       [](Playlist& p) { p.remove(0); }},
      {"set identical track",
       [](Playlist& p) {
         p.add({Track{.title = "A", .provider_meta = {{"id", "1"}}}});
         (void)p.track(0);
       },
       [](Playlist& p) { p.set_track(0, *p.track(0)); }},
      {"set invalid track",
       [](Playlist&) {},
       [](Playlist& p) { p.set_track(0, Track{.title = "A"}); }},
      {"toggle invalid bookmark",
       [](Playlist&) {},
       [](Playlist& p) { p.toggle_bookmark(0); }},
      {"set same repeat mode",
       [](Playlist&) {},
       [](Playlist& p) { p.set_repeat(RepeatMode::Off); }},
      {"restore matching snapshot",
       [](Playlist& p) { (void)p.snapshot(); },
       [](Playlist& p) { p.restore(p.snapshot()); }},
  };

  for (const Case& c : cases) {
    SECTION(c.name) {
      Playlist p;
      c.prep(p);
      const std::uint64_t before = p.revision();
      c.act(p);
      REQUIRE(p.revision() == before);
    }
  }
}

TEST_CASE("RevisionUnchangedByReadOnlyCalls", "[playlist]") {
  Playlist p;
  p.add({Track{.title = "A"}, Track{.title = "B"}});
  p.queue(1);
  const std::uint64_t before = p.revision();

  p.revision();
  p.len();
  p.current();
  p.index();
  p.current_is_queued();
  p.peek_next();
  p.queue_position(1);
  p.queue_len();
  p.queue_tracks();
  p.queue_window(0, 1);
  p.snapshot();
  p.tracks();
  p.track(0);
  p.track_window(0, 1);
  p.bookmark_count();
  p.shuffled();
  p.repeat();

  REQUIRE(p.revision() == before);
}

// ============================================================================
// shuffle_repeat_test.go
// ============================================================================
TEST_CASE("ToggleShuffle", "[playlist]") {
  auto p = make_playlist(5, false);
  p->set_index(2);  // C

  REQUIRE_FALSE(p->shuffled());

  p->toggle_shuffle();
  REQUIRE(p->shuffled());

  auto [cur, _] = p->current();
  REQUIRE(cur.title == "C");
}

TEST_CASE("ToggleShuffleOff", "[playlist]") {
  auto p = make_playlist(5, true);  // start shuffled

  auto [cur_track, _] = p->current();

  p->toggle_shuffle();  // turn off

  REQUIRE_FALSE(p->shuffled());

  auto [cur2, _2] = p->current();
  REQUIRE(cur2.title == cur_track.title);

  // Playback follows sequential order from current track onward.
  for (int i = 0; i < 4; ++i) {
    REQUIRE(p->next().second);
  }
}

TEST_CASE("ToggleShuffleEmpty", "[playlist]") {
  Playlist p;
  p.toggle_shuffle();  // should not panic
  REQUIRE(p.shuffled());
}

TEST_CASE("CycleRepeat", "[playlist]") {
  Playlist p;

  REQUIRE(p.repeat() == RepeatMode::Off);

  p.cycle_repeat();
  REQUIRE(p.repeat() == RepeatMode::All);

  p.cycle_repeat();
  REQUIRE(p.repeat() == RepeatMode::One);

  p.cycle_repeat();
  REQUIRE(p.repeat() == RepeatMode::Off);
}

TEST_CASE("SetRepeat", "[playlist]") {
  Playlist p;

  p.set_repeat(RepeatMode::One);
  REQUIRE(p.repeat() == RepeatMode::One);

  p.set_repeat(RepeatMode::All);
  REQUIRE(p.repeat() == RepeatMode::All);
}

TEST_CASE("ShufflePreservesAllTracks", "[playlist]") {
  auto p = make_playlist(10, false);
  p->set_repeat(RepeatMode::All);
  p->toggle_shuffle();

  std::set<std::string> seen;
  auto [cur, _] = p->current();
  seen.insert(cur.title);

  for (int i = 0; i < 9; ++i) {
    auto [next, ok] = p->next();
    REQUIRE(ok);
    REQUIRE(seen.find(next.title) == seen.end());
    seen.insert(next.title);
  }
  REQUIRE(seen.size() == 10);
}

TEST_CASE("SetTrack", "[playlist]") {
  auto p = make_playlist(3, false);

  p->set_track(1, Track{.title = "NEW"});

  REQUIRE(p->tracks()[1].title == "NEW");
}

TEST_CASE("SetTrackOutOfBounds", "[playlist]") {
  auto p = make_playlist(3, false);

  p->set_track(-1, Track{.title = "X"});
  p->set_track(5, Track{.title = "X"});

  REQUIRE(p->tracks()[0].title == "A");
}

TEST_CASE("ToggleBookmark", "[playlist]") {
  auto p = make_playlist(3, false);

  p->toggle_bookmark(0);
  REQUIRE(p->tracks()[0].bookmark);
  REQUIRE(p->bookmark_count() == 1);

  p->toggle_bookmark(0);
  REQUIRE_FALSE(p->tracks()[0].bookmark);
  REQUIRE(p->bookmark_count() == 0);
}

TEST_CASE("ToggleBookmarkOutOfBounds", "[playlist]") {
  auto p = make_playlist(3, false);

  p->toggle_bookmark(-1);
  p->toggle_bookmark(5);

  REQUIRE(p->bookmark_count() == 0);
}

TEST_CASE("BookmarkCount", "[playlist]") {
  auto p = make_playlist(5, false);

  p->toggle_bookmark(0);
  p->toggle_bookmark(2);
  p->toggle_bookmark(4);

  REQUIRE(p->bookmark_count() == 3);
}

TEST_CASE("BookmarkCountStaysCoherentAcrossMutations", "[playlist]") {
  Playlist p;
  p.replace({Track{.title = "A", .bookmark = true},
             Track{.title = "B"},
             Track{.title = "C", .bookmark = true}});
  REQUIRE(p.bookmark_count() == 2);

  p.add({Track{.title = "D", .bookmark = true}, Track{.title = "E"}});
  REQUIRE(p.bookmark_count() == 3);

  p.set_track(0, Track{.title = "A"});
  p.set_track(1, Track{.title = "B", .bookmark = true});
  REQUIRE(p.bookmark_count() == 3);

  REQUIRE(p.move(1, 3));
  REQUIRE(p.bookmark_count() == 3);
  Snapshot snapshot = p.snapshot();

  REQUIRE(p.remove(2));
  REQUIRE(p.bookmark_count() == 2);

  p.replace({});
  REQUIRE(p.bookmark_count() == 0);
  p.restore(snapshot);
  REQUIRE(p.bookmark_count() == 3);
}

// ============================================================================
// ownership_test.go
// ============================================================================
TEST_CASE("ReplaceOwnsInputTracks", "[playlist]") {
  Track input = Track{.title = "Original",
                      .bookmark = true,
                      .provider_meta = {{"provider.id", "original"}}};
  Playlist p;
  p.replace({input});

  input.title = "Changed";
  input.bookmark = false;
  input.provider_meta["provider.id"] = "changed";

  Track got = p.tracks()[0];
  REQUIRE(got.title == "Original");
  REQUIRE(got.bookmark);
  REQUIRE(got.provider_meta["provider.id"] == "original");
  REQUIRE(p.bookmark_count() == 1);

  auto returned = p.tracks();
  returned[0].bookmark = false;
  returned[0].provider_meta["provider.id"] = "returned";
  REQUIRE(p.bookmark_count() == 1);
  REQUIRE(p.tracks()[0].provider_meta["provider.id"] == "original");
}

TEST_CASE("TrackInputsAndSnapshotsOwnProviderMeta", "[playlist]") {
  Playlist p;
  Track added = Track{.title = "Added", .provider_meta = {{"provider.id", "added"}}};
  p.add({added});
  added.provider_meta["provider.id"] = "changed";
  REQUIRE(p.tracks()[0].provider_meta["provider.id"] == "added");

  Track replacement = Track{.title = "Replacement",
                            .provider_meta = {{"provider.id", "replacement"}}};
  p.set_track(0, replacement);
  replacement.provider_meta["provider.id"] = "changed";
  REQUIRE(p.tracks()[0].provider_meta["provider.id"] == "replacement");

  auto tracks = p.tracks();
  tracks[0].provider_meta["provider.id"] = "snapshot";
  REQUIRE(p.tracks()[0].provider_meta["provider.id"] == "replacement");
}

TEST_CASE("SnapshotAndRestoreOwnProviderMeta", "[playlist]") {
  Playlist p;
  p.replace({Track{.provider_meta = {{"provider.id", "original"}}}});

  Snapshot snapshot = p.snapshot();
  snapshot.tracks[0].provider_meta["provider.id"] = "snapshot";
  REQUIRE(p.tracks()[0].provider_meta["provider.id"] == "original");

  snapshot = p.snapshot();
  p.restore(snapshot);
  snapshot.tracks[0].provider_meta["provider.id"] = "after-restore";
  REQUIRE(p.tracks()[0].provider_meta["provider.id"] == "original");
}

TEST_CASE("ReturnedTrackOwnsProviderMeta", "[playlist]") {
  Playlist p;
  p.replace({Track{.provider_meta = {{"provider.id", "original"}}}});

  auto [track, index] = p.current();
  REQUIRE(index == 0);
  track.provider_meta["provider.id"] = "changed";

  REQUIRE(p.tracks()[0].provider_meta["provider.id"] == "original");
}

// ============================================================================
// url_test.go
// ============================================================================
TEST_CASE("IsURL", "[playlist]") {
  struct Case { const char* path; bool want; };
  const Case cases[] = {
      {"http://example.com/stream.mp3", true},
      {"https://example.com/stream.mp3", true},
      {"ytsearch:lofi hip hop", true},
      {"ytsearch1:some song", true},
      {"ytsearch10:multi result query", true},
      {"scsearch:artist name", true},
      {"scsearch1:track name", true},
      {"scsearch10:multi result query", true},
      {"ytsearchabc:bad", false},
      {"/home/user/music/song.mp3", false},
      {"relative/path.flac", false},
      {"", false},
      {"ftp://files.example.com/song.mp3", false},
      {"spotify:track:abc123", false},
  };
  for (const Case& c : cases) {
    SECTION(c.path) {
      REQUIRE(bootamp::playlist::is_url(c.path) == c.want);
    }
  }
}

TEST_CASE("IsM3U", "[playlist]") {
  struct Case { const char* path; bool want; };
  const Case cases[] = {
      {"https://example.com/playlist.m3u", true},
      {"https://example.com/playlist.m3u8", true},
      {"https://example.com/playlist.M3U", true},
      {"/home/user/playlist.m3u", true},
      {"/home/user/playlist.m3u8", true},
      {"https://example.com/stream.mp3", false},
      {"/home/user/song.mp3", false},
      {"", false},
  };
  for (const Case& c : cases) {
    SECTION(c.path) {
      REQUIRE(bootamp::playlist::is_m3u(c.path) == c.want);
    }
  }
}

TEST_CASE("IsLocalM3U", "[playlist]") {
  struct Case { const char* path; bool want; };
  const Case cases[] = {
      {"/home/user/playlist.m3u", true},
      {"/home/user/playlist.m3u8", true},
      {"https://example.com/playlist.m3u", false},
      {"/home/user/song.mp3", false},
  };
  for (const Case& c : cases) {
    SECTION(c.path) {
      REQUIRE(bootamp::playlist::is_local_m3u(c.path) == c.want);
    }
  }
}

TEST_CASE("IsPLS", "[playlist]") {
  struct Case { const char* path; bool want; };
  const Case cases[] = {
      {"https://example.com/station.pls", true},
      {"https://example.com/station.PLS", true},
      {"/home/user/station.pls", true},
      {"https://example.com/stream.mp3", false},
      {"/home/user/song.mp3", false},
  };
  for (const Case& c : cases) {
    SECTION(c.path) {
      REQUIRE(bootamp::playlist::is_pls(c.path) == c.want);
    }
  }
}

TEST_CASE("IsLocalPLS", "[playlist]") {
  struct Case { const char* path; bool want; };
  const Case cases[] = {
      {"/home/user/station.pls", true},
      {"https://example.com/station.pls", false},
      {"/home/user/song.mp3", false},
  };
  for (const Case& c : cases) {
    SECTION(c.path) {
      REQUIRE(bootamp::playlist::is_local_pls(c.path) == c.want);
    }
  }
}

TEST_CASE("IsYouTubeURL", "[playlist]") {
  struct Case { const char* path; bool want; };
  const Case cases[] = {
      {"https://www.youtube.com/watch?v=dQw4w9WgXcQ", true},
      {"https://youtube.com/watch?v=abc123", true},
      {"https://youtu.be/abc123", true},
      {"https://m.youtube.com/watch?v=abc123", true},
      {"https://music.youtube.com/watch?v=abc123", false},
      {"ytsearch:lofi hip hop", false},
      {"ytsearch1:some song", false},
      {"ytsearch10:multi result query", false},
      {"https://soundcloud.com/artist/track", false},
      {"/local/file.mp3", false},
      {"", false},
  };
  for (const Case& c : cases) {
    SECTION(c.path) {
      REQUIRE(bootamp::playlist::is_youtube_url(c.path) == c.want);
    }
  }
}

TEST_CASE("IsYouTubeMusicURL", "[playlist]") {
  struct Case { const char* path; bool want; };
  const Case cases[] = {
      {"https://music.youtube.com/watch?v=abc123", true},
      {"https://www.music.youtube.com/watch?v=abc123", true},
      {"https://www.youtube.com/watch?v=abc123", false},
      {"https://youtu.be/abc123", false},
      {"/local/file.mp3", false},
  };
  for (const Case& c : cases) {
    SECTION(c.path) {
      REQUIRE(bootamp::playlist::is_youtube_music_url(c.path) == c.want);
    }
  }
}

TEST_CASE("IsYTDL", "[playlist]") {
  struct Case { const char* path; bool want; };
  const Case cases[] = {
      {"https://www.youtube.com/watch?v=abc123", true},
      {"https://youtu.be/abc123", true},
      {"https://music.youtube.com/watch?v=abc123", true},
      {"ytsearch:lofi hip hop", true},
      {"ytsearch1:some song", true},
      {"ytsearch10:multi result query", true},
      {"scsearch:artist name", true},
      {"scsearch1:track name", true},
      {"scsearch5:multi result query", true},
      {"https://soundcloud.com/artist/track", true},
      {"https://bandcamp.com/album", true},
      {"https://artist.bandcamp.com/album/name", true},
      {"https://bilibili.com/video/BV123", true},
      {"https://www.bilibili.com/video/BV123", true},
      {"https://space.bilibili.com/12345", true},
      {"https://b23.tv/abc123", true},
      {"https://music.163.com/song?id=12345", true},
      {"https://example.com/stream.mp3", false},
      {"/local/file.mp3", false},
      {"", false},
  };
  for (const Case& c : cases) {
    SECTION(c.path) {
      REQUIRE(bootamp::playlist::is_ytdl(c.path) == c.want);
    }
  }
}

TEST_CASE("IsFeed", "[playlist]") {
  struct Case { const char* path; bool want; };
  const Case cases[] = {
      {"https://example.com/feed.xml", true},
      {"https://example.com/podcast.rss", true},
      {"https://example.com/feed.atom", true},
      {"https://example.com/podcast.XML", true},
      {"https://example.com/stream.mp3", false},
      {"/local/feed.xml", false},
      {"", false},
  };
  for (const Case& c : cases) {
    SECTION(c.path) {
      REQUIRE(bootamp::playlist::is_feed(c.path) == c.want);
    }
  }
}

TEST_CASE("IsXiaoyuzhouEpisode", "[playlist]") {
  struct Case { const char* path; bool want; };
  const Case cases[] = {
      {"https://www.xiaoyuzhoufm.com/episode/abc123", true},
      {"https://xiaoyuzhoufm.com/episode/abc123", true},
      {"https://m.xiaoyuzhoufm.com/episode/abc123", true},
      {"https://www.xiaoyuzhoufm.com/podcast/abc123", false},
      {"https://example.com/episode/abc123", false},
      {"/local/file.mp3", false},
  };
  for (const Case& c : cases) {
    SECTION(c.path) {
      REQUIRE(bootamp::playlist::is_xiaoyuzhou_episode(c.path) == c.want);
    }
  }
}

// ============================================================================
// track_test.go (playlist-module portion)
// ============================================================================
TEST_CASE("TrackMeta", "[playlist]") {
  SECTION("empty map returns empty") {
    Track tr{.title = "Test"};
    REQUIRE(tr.meta("navidrome.id").empty());
  }
  SECTION("existing key") {
    Track tr{.title = "Test", .provider_meta = {{"navidrome.id", "abc123"}}};
    REQUIRE(tr.meta("navidrome.id") == "abc123");
  }
  SECTION("missing key") {
    Track tr{.title = "Test", .provider_meta = {{"navidrome.id", "abc123"}}};
    REQUIRE(tr.meta("jellyfin.id").empty());
  }
}

TEST_CASE("TrackDisplayName", "[playlist]") {
  SECTION("artist and title") {
    Track tr{.title = "Creep", .artist = "Radiohead"};
    REQUIRE(tr.display_name() == "Radiohead - Creep");
  }
  SECTION("title only") {
    Track tr{.title = "Unknown Song"};
    REQUIRE(tr.display_name() == "Unknown Song");
  }
  SECTION("empty") {
    Track tr;
    REQUIRE(tr.display_name().empty());
  }
}

TEST_CASE("TrackIsLive", "[playlist]") {
  REQUIRE(Track{.realtime = true}.is_live());
  REQUIRE_FALSE(Track{.realtime = false}.is_live());
}

TEST_CASE("TrackIsAlbum", "[playlist]") {
  SECTION("album kind") {
    Track tr{.title = "Album",
             .provider_meta = {{"kind", "album"}, {"albumID", "xyz"}}};
    REQUIRE(tr.is_album());
    REQUIRE(tr.album_id() == "xyz");
  }
  SECTION("plain track") {
    Track tr{.title = "Song"};
    REQUIRE_FALSE(tr.is_album());
    REQUIRE(tr.album_id().empty());
  }
}

TEST_CASE("TrackFromURL", "[playlist]") {
  struct Case {
    const char* name;
    const char* url;
    const char* want_title;
  };
  const Case cases[] = {
      {"with filename", "https://example.com/music/song.mp3", "song"},
      {"stream path fallback to hostname", "https://radio.example.com/stream",
       "radio.example.com"},
      {"rest path fallback to hostname", "https://api.example.com/rest",
       "api.example.com"},
      {"query params ignored", "https://example.com/song.mp3?token=abc", "song"},
      {"root path fallback to hostname", "https://radio.example.com/", "radio.example.com"},
  };
  for (const Case& c : cases) {
    SECTION(c.name) {
      Track tr = bootamp::playlist::track_from_path(c.url);
      REQUIRE(tr.title == c.want_title);
      REQUIRE(tr.stream);
      REQUIRE(tr.path == c.url);
    }
  }
}

TEST_CASE("RepeatModeString", "[playlist]") {
  struct Case { RepeatMode mode; std::string_view want; };
  const Case cases[] = {
      {RepeatMode::Off, "Off"},
      {RepeatMode::All, "All"},
      {RepeatMode::One, "One"},
      {static_cast<RepeatMode>(99), "Off"},  // unknown defaults to "Off"
  };
  for (const Case& c : cases) {
    SECTION(std::string(c.want)) {
      REQUIRE(bootamp::playlist::to_string(c.mode) == c.want);
    }
  }
}

TEST_CASE("CycleRepeatFreeFunction", "[playlist]") {
  REQUIRE(bootamp::playlist::cycle_repeat(RepeatMode::Off) == RepeatMode::All);
  REQUIRE(bootamp::playlist::cycle_repeat(RepeatMode::All) == RepeatMode::One);
  REQUIRE(bootamp::playlist::cycle_repeat(RepeatMode::One) == RepeatMode::Off);
}
