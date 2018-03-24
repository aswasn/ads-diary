function get_diary_content() {
    const d_id = $.getUrlParam("diary_id");
    $.ajax({
        url: "api/get_diary_content",
        method: "post",
        data: {diary_id: d_id},
        dataType: "json"
    }).done(function(msg) {
        $("#content,#edit-textarea").text(msg.content);
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
        console.log(msg);
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
        console.log(msg);
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
            user_id: u_id
        },
        dataType: "json"
    }).done(function(msg) {
        console.log(msg);
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
            user_id: u_id
        },
        dataType: "json"
    }).done(function(msg) {
        console.log(msg);
    });
}

$(function() {
    get_diary_content();
    get_comments();
    $("#send-comment-btn").on("click", send_comment);
    $("#like-btn").on("click", like);
    $("#submit-edit-btn").on("click", submit_edit);
});
