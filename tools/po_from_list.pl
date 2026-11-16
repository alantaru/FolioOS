#!/usr/bin/perl
# FolioOS — monta um .po a partir do modelo (folio.pot) e de uma lista numerada de traduções.
#
#   perl tools/po_from_list.pl list   assets/i18n/folio.pot                  > lista numerada (inglês)
#   perl tools/po_from_list.pl build  assets/i18n/folio.pot header.txt trad.txt > idioma.po
#
# trad.txt: uma linha por texto, "N<TAB>tradução"; plurais com as formas separadas por " || ".
# "\n" na tradução vira quebra de linha. Linhas que começam com # são ignoradas.
# Útil para quem traduz em lote; para o dia a dia, edite o .po direto (Poedit, Weblate…).
use strict; use warnings;
binmode STDOUT, ':raw';

sub read_pot {
    my ($path) = @_;
    open my $h, '<:raw', $path or die "$path: $!";
    my (@e, $cur, $field);
    while (my $l = <$h>) {
        $l =~ s/\r?\n$//;
        if ($l =~ /^#:\s*(.*)/) { $cur = { refs => $1 }; next; }
        if ($l =~ /^msgid_plural "(.*)"$/) { $cur->{plural} = unesc($1); $field = 'plural'; next; }
        if ($l =~ /^msgid "(.*)"$/) { $cur //= {}; $cur->{id} = unesc($1); $field = 'id'; next; }
        if ($l =~ /^msgstr/) { $field = ''; if (defined $cur && length($cur->{id} // '')) { push @e, $cur; } $cur = undef; next; }
        if ($l =~ /^"(.*)"$/ && $field) { $cur->{$field} .= unesc($1); next; }
    }
    return @e;
}
sub unesc { my $s = shift; $s =~ s/\\(.)/$1 eq 'n' ? "\n" : $1 eq 't' ? "\t" : $1/ge; return $s; }
sub esc {
    my $s = shift;
    $s =~ s/\\/\\\\/g; $s =~ s/"/\\"/g; $s =~ s/\t/\\t/g;
    if ($s =~ /\n./) { my @p = split /(?<=\n)/, $s; return "\"\"\n" . join("\n", map { (my $x = $_) =~ s/\n/\\n/g; "\"$x\"" } @p); }
    $s =~ s/\n/\\n/g;
    return "\"$s\"";
}

my $cmd = shift // '';
if ($cmd eq 'list') {
    my @e = read_pot(shift);
    my $i = 0;
    for my $e (@e) {
        $i++;
        (my $id = $e->{id}) =~ s/\n/\\n/g;
        my $pl = defined $e->{plural} ? " || $e->{plural}" : '';
        print "$i\t$id$pl\n";
    }
    exit 0;
}
if ($cmd eq 'build') {
    my ($pot, $header, $list) = @ARGV;
    my @e = read_pot($pot);
    my %tr;
    open my $h, '<:raw', $list or die "$list: $!";
    while (my $l = <$h>) {
        $l =~ s/\r?\n$//;
        next if $l =~ /^#/ || $l !~ /\S/;
        my ($n, $t) = split /\t/, $l, 2;
        die "linha sem número: $l\n" unless $n =~ /^\d+$/ && defined $t;
        die "número $n repetido\n" if exists $tr{$n};
        $t =~ s/\\n/\n/g;
        $tr{$n} = $t;
    }
    open my $hh, '<:raw', $header or die "$header: $!";
    local $/; my $hdr = <$hh>;
    print "msgid \"\"\nmsgstr \"\"\n";
    for my $line (split /\n/, $hdr) { next unless $line =~ /\S/; $line =~ s/"/\\"/g; print "\"$line\\n\"\n"; }
    print "\n";
    my $i = 0;
    for my $e (@e) {
        $i++;
        my $t = $tr{$i};
        print "#: $e->{refs}\n" if $e->{refs};
        print "#, c-format\n" if $e->{id} =~ /%/ || (defined $e->{plural} && $e->{plural} =~ /%/);
        print "msgid ", esc($e->{id}), "\n";
        if (defined $e->{plural}) {
            print "msgid_plural ", esc($e->{plural}), "\n";
            my @f = defined $t ? split(/ \|\| /, $t) : ('', '');
            for my $k (0 .. $#f) { print "msgstr[$k] ", esc($f[$k]), "\n"; }
        } else {
            print "msgstr ", esc(defined $t ? $t : ''), "\n";
        }
        print "\n";
    }
    my @missing = grep { !exists $tr{$_} } 1 .. scalar @e;
    print STDERR "po_from_list: ", scalar(@e) - @missing, " de ", scalar(@e), " traduzidos", (@missing ? " (faltam: @missing[0 .. ($#missing > 9 ? 9 : $#missing)])" : ""), "\n";
    exit 0;
}
die "uso: po_from_list.pl list <pot> | build <pot> <header> <lista>\n";
