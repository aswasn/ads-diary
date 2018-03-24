#include <string>
#include "json.hpp"
using json = nlohmann::json;

namespace objects {
    struct diary {
        int id;
        std::string user;
        std::string content;
        uint64_t utime;
    };


    struct comment {
        int id;
        std::string user;
        int diary_id;
        std::string content;
    };


    void to_json(json& j, const diary& d) {
        j = json{{"id", d.id}, {"user", d.user}, {"content", d.content}, {"utime", d.utime}};
    }

    void from_json(const json& j, diary& d) {
        d.id = j.at("id").get<int>();
        d.user = j.at("user").get<std::string>();
        d.content = j.at("content").get<std::string>();
        d.utime = j.at("utime").get<uint64_t>();
    }

    void to_json(json& j, const comment& c) {
        j = json{{"id", c.id}, {"user", c.user}, {"content", c.content}, {"diary_id", c.diary_id}};
    }

    void from_json(const json& j, comment& c) {
        c.id = j.at("id").get<int>();
        c.user = j.at("user").get<std::string>();
        c.content = j.at("content").get<std::string>();
        c.diary_id = j.at("diary_id").get<uint64_t>();
    }

}
