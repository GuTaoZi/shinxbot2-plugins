// Offline test of the celesteguessr plugin: a fake bot that prints what would be sent to QQ, a scripted round with
// four players (chapter pick, map once per round, guesses, *cg check, early end), and the reveal picture.
//
//   g++ -std=c++17 -include algorithm -o /tmp/cg_round cg_offline_round.cpp -I../../../lib/shinxbot2-api/include \
//       -I../../../lib/shinxbot2-api/include/meta_func $(Magick++-config --cxxflags --libs) \
//       -L$HOME/shinxbot2/lib -lutils -ljsoncpp -lzip -lssl -lcrypto -lcurl -lfmt -ldl -Wl,-rpath,$HOME/shinxbot2/lib
//   /tmp/cg_round <plugin .so> <dir with config/ and resource/celesteguessr/>
#include "processable.h"
#include "utils.h"
#include <Magick++.h>
#include <chrono>
#include <deque>
#include <dlfcn.h>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define private public // to read the answer of the scripted round (standard headers are already in)
#include "../celesteguessr.h"
#undef private

class fakebot : public bot {
public:
    std::string dir;
    std::map<userid_t, std::string> cards{{1001, "发起人"}, {1002, "小B"}, {1003, "群名片C"}, {1004, "D同学"}};
    explicit fakebot(std::string d) : bot(0, 0, ""), dir(std::move(d)) {}
    void run() override {}
    void cq_send_all_op(const std::string &) override {}
    void input_process(const std::string &) override {}
    bool is_op(const userid_t a) const override { return a == 1001; }
    std::string getConfigDir() const override { return dir + "/config"; }
    std::string getResourceDir() const override { return dir + "/resource"; }
    std::string cq_send(const std::string &message, const msg_meta &conf) const override
    {
        std::cout << "  [bot -> " << conf.message_type << " " << conf.group_id << "] " << message << "\n";
        return "{\"status\":\"ok\"}";
    }
    std::string cq_send(const std::string &end_point, const Json::Value &J) const override
    {
        if (end_point == "get_group_member_info") {
            auto it = cards.find(J["user_id"].asUInt64());
            return std::string("{\"data\":{\"card\":\"") + (it == cards.end() ? "" : it->second) +
                   "\",\"nickname\":\"nick\"}}";
        }
        std::cout << "  [bot " << end_point << "] " << J.toStyledString().substr(0, 80) << "\n";
        return "{\"status\":\"ok\"}";
    }
};

int main(int argc, char **argv)
{
    Magick::InitializeMagick(*argv);
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) {
        std::cerr << dlerror() << "\n";
        return 1;
    }
    auto create = reinterpret_cast<processable *(*)()>(dlsym(h, "create_t"));
    fakebot B(argv[2]);
    auto *cg = static_cast<celesteguessr *>(create());
    std::function<void(bot *)> tick;
    cg->set_callback([&](std::function<void(bot *)> f) { tick = std::move(f); });
    int64_t mid = 0;
    auto say = [&](userid_t u, const std::string &m) {
        std::cout << u << " (" << B.cards[u] << "): " << m << "\n";
        msg_meta conf("group", u, 692267627, ++mid, &B);
        if (cg->check(m, conf)) {
            cg->process(m, conf);
        }
    };
    say(1001, "*cg 开始 60");
    const auto &r = cg->rounds_.at("g692267627");
    const auto &s = cg->sides_.at(r.side);
    const int ans = s.shots[r.shot].room;
    const int near = ans > 1 ? ans - 1 : ans + 1;
    std::string ans_name;
    for (const auto &rm : s.rooms) {
        if (rm.num == ans) {
            ans_name = rm.name;
        }
    }
    std::cout << "== answer: " << r.side << " room " << ans << " (" << ans_name << "), shot "
              << s.shots[r.shot].file << "\n";
    say(1002, "*cg 1A");                         // pick: the map is posted
    say(1003, "*cg 1a");                         // the same chapter: "the map is above"
    say(1002, "*cg " + std::to_string(ans));     // bullseye
    say(1003, "*cg 1A " + std::to_string(near)); // the room before or after on the route
    say(1004, "*cg 2A 5");                       // no data for 2A
    say(1004, "*cg 1A 30");                      // somewhere else in 1A
    say(1003, "*cg 3");                          // already guessed
    say(1001, "*cg check");
    std::cout << "== waiting 11 s for the early end\n";
    std::this_thread::sleep_for(std::chrono::seconds(11));
    tick(&B);
    say(1001, "*cg 排行");
    return 0;
}
