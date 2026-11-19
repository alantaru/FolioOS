#!/usr/bin/perl
# FolioOS — README em todos os idiomas, com o MESMO conteúdo do README.md (inglês, a fonte).
#
# O README.md é dividido em blocos (parágrafos, títulos, listas, tabelas, separados por linha em
# branco; blocos de código ficam como estão). Cada idioma tem docs/i18n/README.<código>.po com a
# tradução de cada bloco — o formato gettext de sempre, editável no Poedit/Weblate. Este script:
#
#   perl tools/readmes.pl            atualiza os .po (blocos novos, mudados, obsoletos) e gera
#                                    README.<código>.md + a barra de idiomas do README.md
#   perl tools/readmes.pl --check    só confere: sai com erro se algum idioma tiver bloco sem tradução
#   perl tools/readmes.pl skeleton   lista os blocos traduzíveis, separados por linhas "@@@"
#   perl tools/readmes.pl import <código> <arquivo>   traz as traduções de um arquivo nesse formato
#
# Bloco sem tradução (ou cujo inglês mudou) sai em inglês no README do idioma, e o script avisa.
# Títulos traduzidos ganham a âncora do título em inglês, então links como #faq funcionam em
# todos os idiomas. Não edite README.<código>.md à mão: edite o .po.
use strict; use warnings; use utf8;
binmode STDOUT, ':encoding(UTF-8)'; binmode STDERR, ':encoding(UTF-8)';

my @LANGS = (
  [ 'en', 'English', 'README.md' ], [ 'es', 'Español' ], [ 'fr', 'Français' ], [ 'de', 'Deutsch' ],
  [ 'it', 'Italiano' ], [ 'pt-BR', 'Português (Brasil)' ], [ 'pt-PT', 'Português (Portugal)' ],
  [ 'nl', 'Nederlands' ], [ 'pl', 'Polski' ], [ 'tr', 'Türkçe' ], [ 'ru', 'Русский' ],
  [ 'uk', 'Українська' ], [ 'zh-CN', '简体中文' ], [ 'ja', '日本語' ], [ 'ko', '한국어' ],
);
my $PODIR = 'docs/i18n';
sub file_of { my $l = shift; for (@LANGS) { return $_->[2] // "README.$_->[0].md" if $_->[0] eq $l } die $l }
sub lang_bar {
    my $cur = shift;
    return '<sub>' . join(' · ', map { $_->[0] eq $cur ? "**$_->[1]**" : "[$_->[1]](" . file_of($_->[0]) . ")" } @LANGS) . '</sub>';
}
sub slurp { my $f = shift; open my $h, '<:raw:encoding(UTF-8)', $f or return undef; local $/; my $s = <$h>; close $h; $s =~ s/\r\n/\n/g; return $s; }
sub spit { my ($f, $s) = @_; open my $h, '>:raw:encoding(UTF-8)', $f or die "$f: $!"; print $h $s; close $h; }

# ------------------------------------------------------------ blocos do README em inglês
sub blocks {
    my $s = shift;
    $s =~ s/<!-- languages -->.*?<!-- \/languages -->/\x{1}LANGBAR\x{1}/s;
    my @lines = split /\n/, $s, -1;
    my (@b, @cur, $code);
    my $flush = sub { if (@cur) { push @b, { text => join("\n", @cur), tr => 1 }; @cur = (); } };
    for my $l (@lines) {
        # blocos de código também se traduzem (comentários, árvore de pastas); comandos ficam iguais
        if (defined $code) { push @$code, $l; if ($l =~ /^\s*```/) { push @b, { text => join("\n", @$code), tr => 1 }; undef $code; } next; }
        if ($l =~ /^\s*```/) { $flush->(); $code = [ $l ]; next; }
        if ($l =~ /^\s*$/) { $flush->(); next; }
        if ($l eq "\x{1}LANGBAR\x{1}") { $flush->(); push @b, { text => $l, tr => 0, bar => 1 }; next; }
        push @cur, $l;
    }
    $flush->();
    push @b, { text => join("\n", @$code), tr => 0 } if $code;
    # só blocos com texto (fora de tags e URLs) ou com alt de imagem precisam de tradução
    for my $x (@b) {
        next unless $x->{tr};
        (my $t = $x->{text}) =~ s/<[^>]*>//g; $t =~ s/\]\([^)]*\)/]/g; $t =~ s/https?:\S+//g;
        $x->{tr} = 0 unless $t =~ /\p{L}{2}/ || $x->{text} =~ /\balt="[^"]*\p{L}{2}/;
    }
    return @b;
}
sub tr_blocks { my @b = grep { $_->{tr} } @_; my %u; return grep { !$u{$_->{text}}++ } @b; }
# âncora do GitHub para um título
sub slug {
    my $h = shift; $h =~ s/^#+\s*//; $h = lc $h;
    $h =~ s/[^\p{L}\p{N}\s_-]//g; $h =~ s/\s/-/g;
    return $h;
}

# ------------------------------------------------------------ .po
sub po_escape { my $s = shift; $s =~ s/\\/\\\\/g; $s =~ s/"/\\"/g; $s =~ s/\t/\\t/g; return $s; }
sub po_str {
    my ($kw, $s) = @_;
    return "$kw \"" . po_escape($s) . "\"\n" if $s !~ /\n/;
    my @p = split /(?<=\n)/, $s;
    return "$kw \"\"\n" . join('', map { my $e = po_escape($_); $e =~ s/\n/\\n/g; "\"$e\"\n" } @p);
}
sub po_unescape { my $s = shift; $s =~ s/\\(.)/$1 eq 'n' ? "\n" : $1 eq 't' ? "\t" : $1/ge; return $s; }
sub read_po {
    my $s = slurp(shift);
    return ({}, {}) unless defined $s;
    my (%tr, %fz);
    for my $e (split /\n\s*\n/, $s) {
        # blocos obsoletos (#~): continuam lidos, para que a tradução antiga sobreviva a novas execuções
        # (é a referência de quem vai traduzir o bloco que mudou) — antes, a 2ª execução as apagava
        my $obs = $e =~ /^#~ msgid/m;
        $e =~ s/^#~ //mg if $obs;
        my ($id, $str, $field, $fuzzy) = (undef, undef, '', $e =~ /^#,.*fuzzy/m);
        for my $l (split /\n/, $e) {
            if ($l =~ /^msgid "(.*)"$/) { $id = po_unescape($1); $field = 'id'; }
            elsif ($l =~ /^msgstr "(.*)"$/) { $str = po_unescape($1); $field = 'str'; }
            elsif ($l =~ /^"(.*)"$/) { if ($field eq 'id') { $id .= po_unescape($1) } elsif ($field eq 'str') { $str .= po_unescape($1) } }
        }
        next unless defined $id && $id ne '' && defined $str;
        $tr{$id} = $str; $fz{$id} = 1 if $fuzzy;
    }
    return (\%tr, \%fz);
}
sub write_po {
    my ($f, $lang, $name, $b, $tr, $fz) = @_;
    my $o = "# FolioOS README — $name\n# Translate each block; keep Markdown, links, image paths and #anchors as they are.\n"
          . "# Generated and updated by tools/readmes.pl from README.md.\n"
          . "msgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\"Language: $lang\\n\"\n\n";
    my %seen;
    for my $x (tr_blocks(@$b)) {
        $seen{$x->{text}} = 1;
        $o .= "#, fuzzy\n" if $fz->{$x->{text}};
        $o .= po_str('msgid', $x->{text}) . po_str('msgstr', $tr->{$x->{text}} // '') . "\n";
    }
    for my $id (sort keys %$tr) {       # blocos que mudaram no inglês: ficam como referência
        next if $seen{$id} || $tr->{$id} eq '';
        my $e = po_str('msgid', $id) . po_str('msgstr', $tr->{$id});
        $e =~ s/^/#~ /mg; $o .= "$e\n";
    }
    spit($f, $o);
}

# ------------------------------------------------------------ comandos
my $readme = slurp('README.md') // die "README.md?\n";
die "README.md: marcadores <!-- languages --> não encontrados\n" unless $readme =~ /<!-- languages -->/;
my @B = blocks($readme);
mkdir $PODIR unless -d $PODIR;
if (@ARGV && $ARGV[0] eq 'skeleton') { print join("\n\@\@\@\n", map { $_->{text} } tr_blocks(@B)), "\n"; exit 0; }
if (@ARGV && $ARGV[0] eq 'import') {
    my ($lang, $file) = @ARGV[1, 2];
    my ($name) = map { $_->[1] } grep { $_->[0] eq $lang } @LANGS or die "idioma desconhecido: $lang\n";
    my @b = tr_blocks(@B);
    my $src = slurp($file) // die "$file?\n";
    my @t = split /\n\@\@\@\n/, $src; s/^\n+|\n+$//g for @t;
    my ($tr, $fz) = read_po("$PODIR/README.$lang.po");
    # o arquivo pode trazer todos os blocos ou só os que ainda faltam no .po (na ordem do README)
    my @miss = grep { my $x = $tr->{ $_->{text} }; !defined $x || $x eq '' || $fz->{ $_->{text} } } @b;
    @b = @miss if @t != @b && @t == @miss;
    die "$file: " . scalar(@t) . " blocos; o README tem " . scalar(tr_blocks(@B)) . " (faltam " . scalar(@miss) . ")\n" if @t != @b;
    for (0 .. $#b) { $tr->{ $b[$_]{text} } = $t[$_]; delete $fz->{ $b[$_]{text} }; }
    write_po("$PODIR/README.$lang.po", $lang, $name, \@B, $tr, $fz);
    print "$lang: " . scalar(@t) . " blocos importados\n";
    exit 0;
}
my $check = @ARGV && $ARGV[0] eq '--check';
my $bad = 0;
for my $L (@LANGS) {
    my ($lang, $name) = @$L;
    next if $lang eq 'en';
    my $pof = "$PODIR/README.$lang.po";
    my ($tr, $fz) = read_po($pof);
    my (@out, $miss);
    for my $x (@B) {
        if ($x->{bar}) { push @out, "<!-- languages -->\n" . lang_bar($lang) . "\n<!-- /languages -->"; next; }
        if (!$x->{tr}) { push @out, $x->{text}; next; }
        my $t = $tr->{ $x->{text} };
        if (!defined $t || $t eq '' || $fz->{ $x->{text} }) { $miss++; $t = $x->{text}; }
        if ($x->{text} =~ /^#+\s/) {                       # título: âncora do inglês para os links
            my $s = slug($x->{text});
            $t = "<a id=\"$s\"></a>\n\n$t" if slug($t) ne $s;
        }
        push @out, $t;
    }
    if ($miss) { $bad++; print STDERR "  $lang: $miss blocos sem tradução (saem em inglês) — edite $pof\n"; }
    next if $check;
    write_po($pof, $lang, $name, \@B, $tr, $fz);
    spit(file_of($lang), "<!-- Generated by tools/readmes.pl from README.md and $pof — edit the .po, not this file. -->\n" . join("\n\n", @out) . "\n");
}
if (!$check) {
    my $bar = "<!-- languages -->\n" . lang_bar('en') . "\n<!-- /languages -->";
    $readme =~ s/<!-- languages -->.*?<!-- \/languages -->/$bar/s;
    spit('README.md', $readme);
}
printf "README.md: %d blocos, %d traduzíveis; %d idiomas%s\n", scalar(@B), scalar(tr_blocks(@B)), scalar(@LANGS) - 1, $bad ? ", $bad com blocos faltando" : ", todos completos";
exit($check && $bad ? 1 : 0);
