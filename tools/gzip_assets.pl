#!/usr/bin/env perl
use strict;
use warnings;
use File::Find;
use File::Spec;
use Getopt::Long;
use IO::Compress::Gzip qw(gzip $GzipError);

my $dirs = 'sprites,sound,sounds,overviews,models,maps,gfx';
my $types = 'bsp,nav,res,wad,mdl,spr,wav,bmp,tga,txt,htm,html';
my $min_saving = 5.0;
GetOptions(
    'dirs=s'       => \$dirs,
    'types=s'      => \$types,
    'min-saving=f' => \$min_saving,
) && @ARGV == 1 or die "usage: $0 ROOT [--dirs LIST] [--types LIST] [--min-saving PERCENT]\n";
my $root = $ARGV[0];

sub split_list {
    my %set;
    for (split /,/, shift) {
        s/^\s+|\s+$//g;
        s/^\.//;
        $set{lc $_} = 1 if length;
    }
    return \%set;
}

my $dir_set = split_list($dirs);
my $type_set = split_list($types);

sub extension {
    my ($name) = @_;
    return $name =~ /\.([^.\/\\]+)$/ ? lc $1 : '';
}

my @sources;
opendir(my $dh, $root) or die "cannot open $root: $!\n";
for my $name (sort readdir $dh) {
    next if $name eq '.' || $name eq '..';
    my $path = File::Spec->catfile($root, $name);
    if (-f $path && extension($name) eq 'wad' && $type_set->{wad}) {
        push @sources, $path;
    } elsif (-d $path && $dir_set->{lc $name}) {
        find({ no_chdir => 1, wanted => sub {
            push @sources, $File::Find::name
                if -f $File::Find::name && $type_set->{extension($File::Find::name)};
        } }, $path);
    }
}
closedir $dh;

sub current {
    my ($source, $target) = @_;
    my @src = stat $source or return 0;
    my @dst = stat $target or return 0;
    return 0 if $dst[7] < 18 || $dst[9] != $src[9];
    open(my $fh, '<:raw', $target) or return 0;
    seek($fh, -4, 2) or return 0;
    read($fh, my $trailer, 4) == 4 or return 0;
    close $fh;
    return unpack('V', $trailer) == ($src[7] & 0xFFFFFFFF);
}

my ($written, $kept, $skipped, $before, $after) = (0, 0, 0, 0, 0);
for my $source (@sources) {
    my $target = "$source.gz";
    if (current($source, $target)) {
        $kept++;
        next;
    }
    my @stat = stat $source or die "cannot stat $source: $!\n";
    open(my $in, '<:raw', $source) or die "cannot read $source: $!\n";
    my $data = do { local $/; <$in> };
    close $in;
    my $packed;
    gzip(\$data => \$packed, -Level => 9, Minimal => 1) or die "gzip $source: $GzipError\n";
    if (!length($data) || length($packed) > length($data) * (1 - $min_saving / 100)) {
        $skipped++;
        unlink $target if -e $target;
        next;
    }
    my $temp = "$target.tmp";
    open(my $out, '>:raw', $temp) or die "cannot write $temp: $!\n";
    print {$out} $packed or die "cannot write $temp: $!\n";
    close $out or die "cannot write $temp: $!\n";
    utime($stat[8], $stat[9], $temp) or die "cannot set time on $temp: $!\n";
    rename($temp, $target) or die "cannot replace $target: $!\n";
    $written++;
    $before += length $data;
    $after += length $packed;
    printf "%s: %d -> %d KiB\n", $source, length($data) / 1024, length($packed) / 1024;
}

printf "written %d, up to date %d, not worth it %d, saved %.1f MiB on written files\n",
    $written, $kept, $skipped, ($before - $after) / (1024 * 1024);
