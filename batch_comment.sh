#!/bin/bash
echo "输入的地址是${1}，发评论的是${2}"
for ((i=1;i<=5;i++)); do
    curl -d "diary_id=2&content=这是评论${i}&user_id=${2}" "${1}/api/add_comment"
done
