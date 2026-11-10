# Sourced by mise (see mise.toml): set up the ESP-IDF version picked with `eim select`, if eim has installed one.
# Uses the activation script's -e output instead of sourcing it, which would run `eim select` on every prompt.
eim_json="$HOME/.espressif/tools/eim_idf.json"
if [ -f "$eim_json" ]; then
    eim_sel=$(sed -n 's/.*"idfSelectedId": *"\([^"]*\)".*/\1/p' "$eim_json")
    eim_act=$(awk -F'"' -v sel="$eim_sel" '$2 == "activationScript" { s = $4 } $2 == "id" && $4 == sel { print s; exit }' "$eim_json")
    if [ -n "$eim_act" ] && [ -f "$eim_act" ]; then
        while IFS='=' read -r key value; do
            case "$key" in
            PATH) export PATH="$value:$PATH" ;; # IDF tools, Python venv and the idf.py shim
            SYSTEM_PATH | '') ;;                # PATH as it was when eim installed IDF
            *) export "$key=$value" ;;
            esac
        done <<EOF
$(sh "$eim_act" -e)
EOF
    fi
    unset eim_sel eim_act key value
fi
unset eim_json
