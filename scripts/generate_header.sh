#!/bin/bash

function generate {
    echo "#pragma once"
    echo "#include <vector>"
    echo "#include <tuple>"
    echo "#include <string>"
    echo ""
    echo "std::vector<std::tuple<std::string, std::string, std::string>> rule_data = {"

    for f in ../src/rules/*; do
        json_file="$f/data.json"
        if [ -f "$json_file" ]; then

            printf "  {"

            jq -c '.sonar.id, .pmd.id, .description' $json_file | tr '\n' ','

            echo "},"
            
        fi
    done

    echo "};"
}

generate > $1
