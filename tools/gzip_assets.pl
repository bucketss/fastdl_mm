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
my $cleanup = 0;
Getopt::Long::Configure('bundling');
GetOptions(
    'dirs=s'       => \$dirs,
    'types=s'      => \$types,
    'min-saving=f' => \$min_saving,
    'cleanup|c'    => \$cleanup,
) && @ARGV == 1
    or die "usage: $0 ROOT [--dirs LIST] [--types LIST] [--min-saving PERCENT] [-c|--cleanup]\n";
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

my @files;
opendir(my $dh, $root) or die "cannot open $root: $!\n";
for my $name (sort readdir $dh) {
    next if $name eq '.' || $name eq '..';
    my $path = File::Spec->catfile($root, $name);
    if (-f $path) {
        push @files, [$path, 1];
    } elsif (-d $path && $dir_set->{lc $name}) {
        find({ no_chdir => 1, wanted => sub {
            push @files, [$File::Find::name, 0] if -f $File::Find::name;
        } }, $path);
    }
}
closedir $dh;

sub wanted_type {
    my ($path, $top) = @_;
    my $ext = extension($path);
    return $type_set->{$ext} && (!$top || $ext eq 'wad');
}

my @sources = map { $_->[0] } grep { wanted_type(@$_) } @files;

if ($cleanup) {
    my $removed = 0;
    for my $file (@files) {
        my ($path, $top) = @$file;
        my $orphan = $path =~ /\.gz\.tmp$/;
        if (!$orphan && $path =~ /^(.*)\.gz$/) {
            $orphan = wanted_type($1, $top) && !-e $1;
        }
        next unless $orphan;
        unlink $path or die "cannot remove $path: $!\n";
        $removed++;
        print "removed $path\n";
    }
    print "removed $removed orphaned files\n";
}

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
