#include <string>
#include <utility>
#include "json.hpp"
using json = nlohmann::json;

typedef std::pair<int,int> psi_ver_t;

namespace objects {
    struct diary {
        int id;
        psi_ver_t ver;    // version
        std::string user;
        std::string content;
        uint64_t utime;
    };

    struct comment {
        int id;
        int diary_id;
        psi_ver_t ver;    // version
        std::string user;
        std::string content;
    };

    // Siyuan: PSI的like用Redis INCR实现
    struct like {
        int diary_id;
        int num;
        int ver;
    };

    void to_json(json& j, const diary& d) {
        j = json{{"id", d.id}, {"ver1", d.ver.first}, {"ver2", d.ver.second}, {"user", d.user}, {"content", d.content}, {"utime", d.utime}};
    }

    void from_json(const json& j, diary& d) {
        d.id = j.at("id").get<int>();
        d.ver.first = j.at("ver1").get<int>();
        d.ver.second = j.at("ver2").get<int>();
        d.user = j.at("user").get<std::string>();
        d.content = j.at("content").get<std::string>();
        d.utime = j.at("utime").get<uint64_t>();
    }

    void to_json(json& j, const comment& c) {
        j = json{{"id", c.id}, {"ver1", c.ver.first}, {"ver2", c.ver.second}, {"user", c.user}, {"content", c.content}, {"diary_id", c.diary_id}};
    }

    void from_json(const json& j, comment& c) {
        c.id = j.at("id").get<int>();
        c.ver.first = j.at("ver1").get<int>();
        c.ver.second = j.at("ver2").get<int>();
        c.user = j.at("user").get<std::string>();
        c.content = j.at("content").get<std::string>();
        c.diary_id = j.at("diary_id").get<int>();
    }

    void to_json(json& j, const like& l) {
        j = json{{"diary_id", l.diary_id}, {"num", l.num}, {"ver", l.ver}};
    }

    void from_json(const json& j, like& l) {
        l.diary_id = j.at("diary_id").get<int>();
        l.num = j.at("num").get<int>();
        l.ver = j.at("ver").get<int>();
    }
}
