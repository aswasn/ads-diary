#include <string>
#include <utility>
#include "json.hpp"
using json = nlohmann::json;

typedef std::pair<int,int> psi_ver_t;

namespace objects {
    enum obj_type {
        DIARY,
        COMMENT
    };

    struct object {
        object() { }
        object(int id, psi_ver_t ver) {
            this->id = id;
            this->ver =  ver;
        }
        int id;
        psi_ver_t ver;
    };

    struct diary : object {
        std::string user;
        std::string content;
        uint64_t utime;
    };

    struct comment : object {
        comment() { }
        comment(int id, psi_ver_t ver, int did,
                std::string &u, std::string &c)
            : object(id, ver), diary_id(did), user(u), content(c) {
        }
        int diary_id;
        std::string user;
        std::string content;
    };

    // Siyuan: PSI的like用Redis INCR实现
    // struct like {
        // int diary_id;
        // int num;
    // };


    void to_json(json& j, const object& o) {
        j = json{{"id", o.id}, {"ver1", o.ver.first}, {"ver2", o.ver.second}};
    }

    void from_json(const json& j, object& o) {
        o.id = j.at("id").get<int>();
        o.ver.first = j.at("ver1").get<int>();
        o.ver.second = j.at("ver2").get<int>();
    }

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
}
