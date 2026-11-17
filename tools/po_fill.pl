#!/usr/bin/perl
# FolioOS — preenche traduções vazias nos .po a partir de um arquivo simples (UTF-8):
#
#   msgid Texto original em inglês
#   pt-BR Tradução
#   es    Traducción
#   ...                      (uma linha por idioma; linha em branco separa os textos)
#
#   perl tools/po_fill.pl lista.txt assets/i18n
#
# Só preenche msgstr vazios (nunca sobrescreve uma tradução feita por alguém).
use strict; use warnings;
my ($list, $dir) = @ARGV;
die "uso: po_fill.pl <lista.txt> <pasta dos .po>\n" unless $list && $dir;
open my $h, '<:raw', $list or die "$list: $!";
my (%tr, $id);
while (my $l = <$h>) {
    $l =~ s/\r?\n$//;
    next if $l =~ /^#/;
    if ($l eq '') { undef $id; next; }
    if ($l =~ /^msgid (.+)$/) { $id = $1; next; }
    die "$list: tradução sem msgid: $l\n" unless defined $id;
    my ($lang, $t) = $l =~ /^(\S+)\s+(.+)$/ or die "$list: linha inválida: $l\n";
    $tr{$lang}{$id} = $t;
}
close $h;
for my $lang (sort keys %tr) {
    my $f = "$dir/$lang.po";
    open my $p, '<:raw', $f or die "$f: $!"; local $/; my $s = <$p>; close $p;
    my $n = 0;
    for my $id (keys %{ $tr{$lang} }) {
        (my $q = $id) =~ s/(["\\])/\\$1/g;
        (my $t = $tr{$lang}{$id}) =~ s/(["\\])/\\$1/g;
        $n++ if $s =~ s/^(msgid "\Q$q\E"\r?\nmsgstr )""(\r?)$/$1"$t"$2/m;
    }
    open $p, '>:raw', $f or die; print $p $s; close $p;
    print "$lang: $n\n";
}
