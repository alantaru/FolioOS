#!/bin/sh
# FolioOS — monta assets/i18n/<idioma>.po a partir das listas numeradas de i18n_work/
# (lista do idioma + textos comuns, que valem iguais em todos os idiomas quando não traduzidos)
#   sh tools/build_po.sh <pasta_de_trabalho> <idioma>...
set -e
W="$1"; shift
for L in "$@"; do
    awk -F'\t' 'NR==FNR { if ($0 !~ /^#/) seen[$1]=1; print; next } !($1 in seen)' "$W/$L.txt" "$W/common.txt" > "$W/$L.merged"
    perl tools/po_from_list.pl build assets/i18n/folio.pot "$W/$L.hdr" "$W/$L.merged" > "assets/i18n/$L.po"
done
