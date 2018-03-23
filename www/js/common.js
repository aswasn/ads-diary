function check_login() {
    const user = Cookies.get("user");
    if (!user) {
        console.log("no user.");
    } else {
        let name = "nobody";
        switch (user) {
            case "bh" : name = "卜衡"; break;
            case "lzc" : name = "刘志成"; break;
            case "cs" : name = "曹慎"; break;
            case "xhn" : name = "徐海宁"; break;
            case "wsy" : name = "王思源"; break;
            case "wn" : name = "王宁"; break;
        }
        $("#login-entry").text(name);
    }
};

(function($) {
    $.getUrlParam = function(name) {
        var reg = new RegExp("(^|&)"+ name +"=([^&]*)(&|$)");
        var r = window.location.search.substr(1).match(reg);
        if (r != null) {
            return unescape(r[2]);
        }
        return null;
    };
})(jQuery);

$(function(){
    check_login();
});
