#!/usr/bin/perl
# Written by [@saphhic](https://github.com/saphhic)
# Date: October 31st 2026
# Description: Builds a portable lookup table from NetSurf Hubbub's gperf data.

use strict;
use warnings;

my ($input_path, $output_path) = @ARGV;
die "Usage: $0 <element-type.gperf> <output.c>\n"
    unless defined $input_path && defined $output_path;

open my $input, '<', $input_path or die "Cannot read $input_path: $!\n";
my @elements;
my $in_table = 0;
while (my $line = <$input>) {
    if ($line =~ /^%%\s*$/) {
        ++$in_table;
        next;
    }
    next unless $in_table == 1;
    if ($line =~ /^\s*([a-z0-9-]+)\s*,\s*([A-Z_]+)\s*$/) {
        push @elements, [$1, $2];
    }
}
close $input;
die "No element mappings found in $input_path\n" unless @elements;

open my $output, '>', $output_path or die "Cannot write $output_path: $!\n";
print {$output} <<'HEADER';
/* Generated from NetSurf Hubbub's element-type.gperf data. */
#include <string.h>
#include <strings.h>
#include "treebuilder/element-type.h"

static const struct element_type_map wordlist[] = {
HEADER
for my $element (@elements) {
    my ($name, $type) = @$element;
    print {$output} "    { \"$name\", $type },\n";
}
print {$output} <<'FOOTER';
};

const struct element_type_map *hubbub_element_type_generated_lookup(
        const char *text, size_t length) {
    for (size_t index = 0; index < sizeof(wordlist) / sizeof(wordlist[0]);
         ++index) {
        if (strlen(wordlist[index].name) == length &&
            strncasecmp(text, wordlist[index].name, length) == 0)
            return &wordlist[index];
    }
    return NULL;
}
FOOTER
close $output or die "Cannot finish $output_path: $!\n";
