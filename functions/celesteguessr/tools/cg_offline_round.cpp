// Offline test of the celesteguessr plugin: a fake bot that prints what would be sent to QQ, a scripted round with
// four players (chapter pick, map once per round, guesses, *cg check, early end), and the reveal picture.
//
//   g++ -std=c++17 -include algorithm -o /tmp/cg_round cg_offline_round.cpp -I../../../lib/shinxbot2-api/include \
//       -I../../../lib/shinxbot2-api/include/meta_func $(Magick++-config --cxxflags --libs) \
//       -L$HOME/shinxbot2/lib -L$HOME/shinxbot2/plugins/lib/functions -lcelesteguessr -lutils -ljsoncpp -lzip -lssl -lcrypto -lcurl -lfmt -ldl -Wl,-rpath,$HOME/shinxbot2/lib:$HOME/shinxbot2/plugins/lib/functions
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
        if (end_point.find("forward") != std::string::npos) {
            std::cout << "  [bot " << end_point << "] " << J["messages"].size() << " nodes\n";
            for (const auto &n : J["messages"]) {
                std::cout << "    node: " << messageArr_to_string(n["data"]["content"]) << "\n";
            }
            return "{\"status\":\"ok\"}";
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
    auto &r = cg->rounds_.at("g692267627");
    r.side = "1A"; // the script below plays 1A (one section: room numbers along the side)
    r.shot = cg->sides_.at("1A").shots.size() / 3;
    cg->make_question("g692267627", r);
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
    say(1004, "*cg 5Z 5");                       // not a side
    say(1004, "*cg 1A 30");                      // somewhere else in 1A
    say(1003, "*cg 3");                          // already guessed
    say(1001, "*cg check");
    std::cout << "== waiting 11 s for the early end\n";
    std::this_thread::sleep_for(std::chrono::seconds(11));
    tick(&B);
    say(1001, "*cg 排行");

    // a split side, if the data has one: chapter (folded forward), section, room; and the one-step form
    std::string big;
    for (const auto &kv : cg->sides_) {
        if (kv.second.maps.size() > 1) {
            big = kv.first;
            break;
        }
    }
    if (big.empty()) {
        std::cout << "== no split side in the data\n";
        return 0;
    }
    say(1001, "*cg 开始 60");
    auto &r2 = cg->rounds_.at("g692267627");
    r2.side = big; // force the answer onto the split side
    r2.shot = cg->sides_.at(big).shots.size() / 2;
    cg->make_question("g692267627", r2);
    const auto &bs = cg->sides_.at(big);
    const int bn = bs.shots[r2.shot].room;
    int bpart = 1, blocal = 1;
    for (const auto &rm : bs.rooms) {
        if (rm.num == bn) {
            bpart = rm.part;
            blocal = rm.local;
        }
    }
    std::cout << "== answer: " << big << "-" << bpart << " " << blocal << "\n";
    say(1002, "*cg " + big);                    // every section, folded
    say(1002, "*cg " + std::to_string(bpart));  // that section alone
    say(1002, "*cg " + std::to_string(blocal)); // the room: bullseye
    say(1003, "*cg " + big + "-" + std::to_string(bpart) + " " + std::to_string(blocal == 1 ? 2 : blocal - 1));
    say(1004, "*cg " + big + " 1");             // section 1 picked, room not yet
    say(1004, "*cg 99");                        // no such room in section 1
    std::this_thread::sleep_for(std::chrono::seconds(11));
    tick(&B); // 1004 has not guessed: no early end
    say(1001, "*cg check");
    say(1001, "*cg 结束");

    // difficulty: a third of the screen, on 1A; the answer room guessed
    say(1001, "*cg ultra 45");
    auto &r3 = cg->rounds_.at("g692267627");
    r3.side = "1A";
    r3.shot = cg->sides_.at("1A").shots.size() / 2;
    cg->make_question("g692267627", r3);
    std::cout << "== ultra question " << r3.question << " centre " << r3.cx << "," << r3.cy << " (shot "
              << cg->sides_.at("1A").shots[r3.shot].x << "," << cg->sides_.at("1A").shots[r3.shot].y << ")\n";
    say(1002, "*cg 1A " + std::to_string(cg->sides_.at("1A").shots[r3.shot].room));
    say(1001, "*cg 结束");
    return 0;
}
