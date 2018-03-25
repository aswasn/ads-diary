#!/bin/bash
echo "输入的地址是${1}，发评论的是${2}"
for ((i=1;i<=5;i++)); do
    let sum=$i+$3
    curl -d "diary_id=2&content=这是评论$sum&user_id=${2}" "${1}/api/add_comment"
    sleep 0.2
done
