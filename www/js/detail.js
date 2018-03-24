var diary_version = -1;
var like_version = -1;

const user_map = {"bh": "卜衡", "cs": "曹慎", "xhn": "徐海宁", "wsy":  "王思源", "lzc": "刘志成", "wn": "王宁"};

function get_diary_content() {
    const d_id = $.getUrlParam("diary_id");
    const $title = $("#title");
    switch (d_id) {
        case "1": $title.text("科研脱发记录");break;
        case "2": $title.text("朋克养生食谱");break;
        case "3": $title.text("2018新年计划");break;
    }
    $.ajax({
        url: "api/get_diary_content",
        method: "post",
        data: {diary_id: d_id},
        dataType: "json"
    }).done(function(msg) {
        $("#content,#edit-textarea").text(msg.content);
        diary_version = msg.ver;
        console.log("diary_version: " + diary_version);
    });
}

function get_comments() {
    const d_id = $.getUrlParam("diary_id");
    $.ajax({
        url: "api/get_comments",
        method: "post",
        data: {diary_id: d_id},
        dataType: "json"
    }).done(function(msg) {
        const $list = $("#comment-list");
        for (let i = 0; i < msg.length; i++) {
            $list.append("<li class=\"list-group-item\">"+msg[i].content+"("+user_map[msg[i].user]+")"+"</li>");
        }
    });
}

function send_comment() {
    const d_id = $.getUrlParam("diary_id");
    const text = $("#comment-input").val();
    const u_id = Cookies.get("user");
    $.ajax({
        url: "api/add_comment",
        method: "post",
        data: {
            diary_id: d_id,
            content: text,
            user_id: u_id
        },
        dataType: "json"
    }).done(function(msg) {
        location.reload();
    });
}

function submit_edit() {
    const d_id = $.getUrlParam("diary_id");
    const text = $("#edit-textarea").val();
    const u_id = Cookies.get("user");
    $.ajax({
        url: "api/edit_diary",
        method: "post",
        data: {
            diary_id: d_id,
            content: text,
            user_id: u_id,
            snapshot_ver: diary_version
        },
        dataType: "json"
    }).done(function(msg) {
        if (msg.success == 1) {
            $("#edit-success-modal").modal("show");
        } else {
            $("#edit-fail-modal").modal("show");
        }
    });
}

function get_like() {
    const d_id = $.getUrlParam("diary_id");
    $.ajax({
        url: "api/get_like",
        method: "post",
        data: {
            diary_id: d_id
        },
        dataType: "json"
    }).done(function(msg) {
        if (msg.ver != undefined && msg.ver != null) like_version = msg.ver;
        $("#like-num").text(msg.num);
    });
}

function like() {
    const d_id = $.getUrlParam("diary_id");
    const u_id = Cookies.get("user");
    $.ajax({
        url: "api/like",
        method: "post",
        data: {
            diary_id: d_id,
            user_id: u_id,
            snapshot_ver: like_version
        },
        dataType: "json"
    }).done(function(msg) {
        if (msg.success == 1) {
            $("#like-num").text(parseInt($("#like-num").text()) + 1);
        }
        like_version = msg.ver;
    });
}

$(function() {
    get_diary_content();
    get_comments();
    get_like();
    $("#send-comment-btn").on("click", send_comment);
    $("#like-btn").on("click", like);
    $("#submit-edit-btn").on("click", submit_edit);
    $(".reload-btn").on("click", function() { location.reload(); });
});
