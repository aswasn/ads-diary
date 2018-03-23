function login(e) {
    Cookies.set("user", $(e.currentTarget).data("user"), { expires: 1000000 });
    setInterval(location.href="index.html", 500);
}

(function(){
    $(".user-card").on("click", login);
})();
