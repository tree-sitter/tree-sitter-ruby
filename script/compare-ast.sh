#! /bin/bash
set -o pipefail

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

file="$1"
"${SCRIPT_DIR}/print-ast.rb" "$file" > "$file.1"
if [ "$?" != "0" ]; then
   echo "print-ast failure: $file"
   rm "$file.1"
   exit 1
fi
"${SCRIPT_DIR}/../node_modules/tree-sitter-cli/cli.js"  parse "$file" | \
   sed 's/ \[[0-9]\+, [0-9]\+\] - \[[0-9]\+, [0-9]\+\]//' | \
   tr $'\n' $'\t' | sed 's/\(^\|[[:space:]]*\)(comment)//g' | tr $'\t' $'\n' > "$file.2"

if [ "$?" != "0" ]; then
   echo "parse failure: $file"
   rm "$file.1" "$file.2"
   exit 1
fi
diff "$file.1" "$file.2" > "$file.diff"
if [ "$?" != "0" ]; then
   echo "diff: $file"
   exit 1
else
   rm "$file".{diff,1,2}
fi

