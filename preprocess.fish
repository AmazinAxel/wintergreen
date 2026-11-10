#!/usr/bin/env fish
#
# books [book.epub ...]   (packaged as `books` by flake.nix)
#
# Review every EPUB in ~/Downloads/books: title, author, cover, chapters. The
# reviewed EPUB replaces the original in place; the WGB is built in a temp dir,
# uploaded to the homelab and never kept on this machine.
#
# Chapters are decided here, not on the device: the reviewed EPUB carries a
# fresh wg.ncx with final labels ("N: Name"), which the quick menu shows
# verbatim. The converter reads only NCX, so this also gives EPUB3 nav-only
# books a TOC.
#
# With no arguments, EPUBs already reviewed (they contain wg.ncx) are skipped;
# name a file to review it again.

set -g host alec@alechomelab
set -g remote /media/books
set -g src ~/Downloads/books

# Run straight from a checkout: fetch the tools once. The `books` package
# already has all of them on PATH.
for t in unzip zip xmlstarlet chafa epub2wgb ssh tar
    command -q $t; and continue
    if set -q WG_BOOKS_SHELL
        echo "books: missing $t" >&2
        exit 1
    end
    set -gx WG_BOOKS_SHELL 1
    set -l repo (path dirname (path resolve (status filename)))
    exec nix shell nixpkgs#unzip nixpkgs#zip nixpkgs#xmlstarlet nixpkgs#chafa nixpkgs#openssh nixpkgs#gnutar \
        $repo#epub2wgb -c fish (status filename) $argv
end

set -g tmp (mktemp -d)
function cleanup --on-event fish_exit
    rm -rf $tmp
end

# ── helpers ──────────────────────────────────────────────────────────────────

# xmlstarlet select, text output. Falls back to the lenient HTML parser for
# XHTML that is not well-formed XML (undeclared &nbsp;, stray tags).
function xsel
    xmlstarlet sel -T -t $argv[2..-1] $argv[1] 2>/dev/null
    or xmlstarlet sel -T --html -t $argv[2..-1] $argv[1] 2>/dev/null
end

# Resolve an href (relative to zip dir $base, '' = root) to a zip entry name.
function norm -a base href
    set -l rel (string split -m1 '#' -- $href)[1]
    set rel (string unescape --style=url -- $rel)
    set -l parts
    for p in (string split / -- "$base/$rel")
        switch $p
            case '' .
            case ..
                test (count $parts) -gt 0; and set -e parts[-1]
            case '*'
                set -a parts $p
        end
    end
    string join / -- $parts
end

function add_entry -a depth label href base
    test -n "$href"; or return
    set -a g_depth $depth
    set -a g_lab "$label"
    set -a g_path (norm "$base" $href)
    set -a g_frag "$(string split -m1 '#' -- $href)[2]"
end

# Collapse whitespace; ALL-CAPS titles become Title Case.
function tidy -a s
    set s (string replace -ra '\s+' ' ' -- $s | string trim)
    if string match -rq '\p{Lu}{2}' -- $s; and not string match -rq '\p{Ll}' -- $s
        set -l small a an and as at but by for from in into nor of on or the to with
        set -l out
        for w in (string lower -- $s | string split ' ')
            if test (count $out) -gt 0; and contains -- $w $small
                set -a out $w
            else
                set -a out (string upper -- (string sub -l 1 -- $w))(string sub -s 2 -- $w)
            end
        end
        set s (string join ' ' -- $out)
    end
    echo $s
end

function xesc
    string replace -a '&' '&amp;' -- $argv[1] | string replace -a '<' '&lt;' | string replace -a '>' '&gt;' | string replace -a '"' '&quot;'
end

# Directory name on the homelab: must pass booksync.js safeName().
function slug -a s
    set s (string lower -- $s | string replace -ra '[^a-z0-9 ._-]+' '' | string replace -ra '\s+' ' ' | string trim -c ' .-')
    test -n "$s"; or set s book
    string sub -l 100 -- $s
end

# Canonical Roman numeral (upper case) → value.
function roman -a s
    test -n "$s"; and string match -rq '^M{0,3}(CM|CD|D?C{0,3})(XC|XL|L?X{0,3})(IX|IV|V?I{0,3})$' -- $s; or return 1
    set -l v 0
    set -l prev 0
    for c in (string split '' -- $s)[-1..1]
        set -l d (switch $c
            case I; echo 1
            case V; echo 5
            case X; echo 10
            case L; echo 50
            case C; echo 100
            case D; echo 500
            case M; echo 1000
        end)
        if test $d -lt $prev
            set v (math $v - $d)
        else
            set v (math $v + $d)
            set prev $d
        end
    end
    echo $v
end

set -g ones zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen
set -g ords zeroth first second third fourth fifth sixth seventh eighth ninth tenth eleventh twelfth thirteenth fourteenth fifteenth sixteenth seventeenth eighteenth nineteenth
set -g tens _ _ twenty thirty forty fifty sixty seventy eighty ninety
set -g otens _ _ twentieth thirtieth fortieth fiftieth sixtieth seventieth eightieth ninetieth

# "Twenty-One", "third", "ninety" → value.
function wordnum -a w
    set -l p (string lower -- $w | string replace -a - ' ' | string split -n ' ')
    switch (count $p)
        case 1
            if set -l i (contains -i -- $p[1] $ones $ords)
                math "($i - 1) % 20"
                return
            end
            if set -l i (contains -i -- $p[1] $tens $otens)
                set -l v (math "(($i - 1) % 10) * 10")
                test $v -ge 20; and echo $v; and return
            end
        case 2
            set -l i (contains -i -- $p[1] $tens); or return 1
            set -l t (math "($i - 1) * 10")
            test $t -ge 20; or return 1
            set -l k (contains -i -- $p[2] $ones $ords); or return 1
            set -l u (math "($k - 1) % 20")
            test $u -ge 1 -a $u -le 9; or return 1
            math $t + $u
            return
    end
    return 1
end

# Split a TOC label into "<number>\t<name>"; number empty when none.
# Handles "Chapter 7", "CHAPTER VII. Name", "Ch. 3: Name", "3. Name",
# "Chapter Twenty-One", "XII", "Seven: Name". A bare Roman or word numeral
# only counts when "Chapter" preceded it or punctuation / the end follows, so
# "I Meet a Stranger" and "One Day" stay titles.
function chapnum -a label
    set -l rest $label
    set -l had 0
    set -l word '(chapter(?![a-z])|chapter(?=[ivxlcdm]+\b)|chap\.|ch\.)'
    if string match -rqi "^$word" -- $rest
        set rest (string replace -ri "^$word\s*(the\s+(?=\S+(\s*[.:]|\$)))?" '' -- $rest)
        set had 1
    else if string match -rqi "\s$word\s*\S+\.?\$" -- $rest
        # "He rode a black horse. CHAPTER III.": the lead-in is an image
        # caption, not a name. Keep only the number.
        set rest (string replace -ri "^.*\s$word\s*(\S+?)\.?\$" '$2' -- $rest)
        set had 1
    end
    set -l sep '^\s*([.:)\]\-–—,|]|$)'
    set -l num ''
    set -l tail $rest

    if string match -rq '^\d+(?![\d\p{L}])' -- $rest
        set num (string match -r '^\d+' -- $rest | string replace -r '^0+(?=\d)' '')
        set tail (string replace -r '^\d+' '' -- $rest)
    end
    if test -z "$num"; and string match -rqi '^[ivxlcdm]+(?![\p{L}\d])' -- $rest
        set -l tok (string match -ri '^[ivxlcdm]+' -- $rest)
        set -l after (string sub -s (math (string length -- $tok) + 1) -- $rest)
        if test $had = 1; or string match -rq -- $sep $after
            if set -l v (roman (string upper -- $tok)); and test $had = 1 -o $v -le 200
                set num $v
                set tail $after
            end
        end
    end
    if test -z "$num"
        for pat in '^[a-z]+[- ][a-z]+' '^[a-z]+'
            set -l w (string match -ri -- $pat $rest)
            test -n "$w"; or continue
            set -l v (wordnum $w); or continue
            set -l after (string sub -s (math (string length -- $w) + 1) -- $rest)
            if test $had = 1; or string match -rq -- $sep $after
                set num $v
                set tail $after
                break
            end
        end
    end

    set tail (string replace -r '^[\s.:)\]\-–—,|]+' '' -- $tail | string trim)
    printf '%s\t%s\n' "$num" "$tail"
end

# Name for a number-only chapter: taken only from the heading that carries
# that number ("Chapter 3 The Thing") or the heading right after it. Anything
# looser picks up image captions and running banners; no name beats a wrong one.
function subtitle -a file num
    set -l H "local-name()='h1' or local-name()='h2' or local-name()='h3' or local-name()='h4' or local-name()='h5'"
    for row in (xsel $file -m "//*[$H]" -v "normalize-space(.)" -o \t -v "normalize-space(following-sibling::*[1][$H])" -n)
        set -l f (string split -m1 \t -- $row)
        set -l r (string split -m1 \t -- (chapnum (tidy $f[1])))
        test "$r[1]" = "$num"; or continue
        if test -n "$r[2]"
            echo $r[2]
        else if test -n "$f[2]"
            set -l s (string split -m1 \t -- (chapnum (tidy $f[2])))
            test -z "$s[1]"; and echo $s[2]
        end
        return
    end
end

# Unnumbered sections, and the subset dropped by default.
set -g matter_re '^(cover$|title( page)?$|half[- ]title|copyright|(table of )?contents$|dedication|epigraph|acknowledg|about the|also by|by the same|other (books|titles)|praise|newsletter|sign up|join |excerpt|preview|sneak peek|bonus|index$|notes$|endnotes|glossary|bibliography|references$|maps?$|map of|cast of characters|dramatis|foreword|preface|introduction|prologue|epilogue|afterword|postscript|coda$|interlude|appendix|author.?s note|a note on|translator|editor.?s note|(part|book|volume|letter|section|act|scene|canto)\s+[^\s:.]+([:.]|$)|.*project gutenberg)'
set -g junk_re '^(cover$|title( page)?$|half[- ]title|copyright|(table of )?contents$|also by|by the same|other (books|titles)|praise|newsletter|sign up|join |excerpt|preview|sneak peek|bonus|about the publisher|index$|.*project gutenberg)'

# ── one book ─────────────────────────────────────────────────────────────────

function process -a epub
    set -l x $tmp/x
    rm -rf $x
    mkdir -p $x
    if not unzip -qo $epub -d $x
        echo "  not a zip, skipped"
        return 1
    end

    set -l opfrel (xsel $x/META-INF/container.xml -v "(//*[local-name()='rootfile']/@full-path)[1]")
    if test -z "$opfrel"; or not test -f "$x/$opfrel"
        echo "  no OPF, skipped"
        return 1
    end
    set -l opf $x/$opfrel
    set -l od (path dirname $opfrel)
    test "$od" = .; and set od ''
    set -l odp ''
    test -n "$od"; and set odp "$od/"

    # Manifest and spine as parallel lists (zip entry names).
    set -l m_id
    set -l m_path
    set -l m_type
    set -l m_prop
    for line in (xmlstarlet sel -T -t -m "//*[local-name()='manifest']/*[local-name()='item']" \
            -v @id -o \t -v @href -o \t -v @media-type -o \t -v @properties -n $opf)
        set -l f (string split \t -- $line)
        set -a m_id "$f[1]"
        set -a m_path (norm "$od" "$f[2]")
        set -a m_type "$f[3]"
        set -a m_prop "$f[4]"
    end
    set -l s_id (xmlstarlet sel -T -t -m "//*[local-name()='spine']/*[local-name()='itemref']" -v @idref -n $opf)
    set -l s_path
    for id in $s_id
        if set -l i (contains -i -- $id $m_id)
            set -a s_path $m_path[$i]
        else
            set -a s_path ''
        end
    end
    if test (count $s_id) -eq 0
        echo "  empty spine, skipped"
        return 1
    end

    # ── title / author ──
    set -l M "//*[local-name()='metadata']"
    set -l title (xmlstarlet sel -T -t -v "normalize-space(($M/*[local-name()='title'])[1])" $opf)
    set -l author (xmlstarlet sel -T -t -v "normalize-space(($M/*[local-name()='creator'])[1])" $opf)
    read -P 'title  ' -c "$title" title; or exit 1
    read -P 'author ' -c "$author" author; or exit 1

    # ── cover: same precedence as the converter ──
    set -l cid (xmlstarlet sel -T -t -v "($M/*[local-name()='meta'][@name='cover']/@content)[1]" $opf)
    set -l cover ''
    set -l best 0
    for i in (seq (count $m_id))
        string match -q 'image/*' -- $m_type[$i]; or continue
        set -l rank 0
        if test -n "$cid" -a "$m_id[$i]" = "$cid"
            set rank 4
        else if string match -q '*cover-image*' -- $m_prop[$i]
            set rank 3
        else if test "$m_id[$i]" = cover
            set rank 2
        else if string match -qi 'cover*' -- (path basename $m_path[$i])
            set rank 1
        end
        if test $rank -gt $best
            set best $rank
            set cover $x/$m_path[$i]
        end
    end
    set -l newcover ''
    while true
        set -l show $cover
        test -n "$newcover"; and set show $newcover
        if test -n "$show"
            chafa --colors=2 --dither=diffusion --size=40x30 $show
        else
            echo '  (no cover)'
        end
        read -P 'cover (enter keep, or image path) ' c; or exit 1
        test -z "$c"; and break
        set c (string trim -c "'\" " -- $c | string replace -r '^~' $HOME)
        if not test -f "$c"
            echo '  not a file'
        else if not string match -qri '\.(jpe?g|png)$' -- $c
            echo '  jpg or png only'
        else
            set newcover (path resolve $c)
        end
    end

    # ── chapters: NCX, else EPUB3 nav, else headings ──
    set -g g_depth
    set -g g_lab
    set -g g_path
    set -g g_frag
    set -l ncx ''
    for i in (seq (count $m_id))
        test "$m_type[$i]" = application/x-dtbncx+xml; and set ncx $m_path[$i]; and break
    end
    if test -n "$ncx"
        set -l base (path dirname $ncx)
        test "$base" = .; and set base ''
        for line in (xsel $x/$ncx -m "//*[local-name()='navPoint']" -v "count(ancestor::*[local-name()='navPoint'])" \
                -o \t -v "normalize-space(*[local-name()='navLabel'])" -o \t -v "*[local-name()='content']/@src" -n)
            set -l f (string split \t -- $line)
            add_entry "$f[1]" "$f[2]" "$f[3]" "$base"
        end
    end
    if test (count $g_lab) -eq 0
        for i in (seq (count $m_id))
            string match -q '*nav*' -- $m_prop[$i]; or continue
            set -l base (path dirname $m_path[$i])
            test "$base" = .; and set base ''
            for line in (xsel $x/$m_path[$i] -m "//*[local-name()='nav'][contains(concat(' ',@*[local-name()='type' or name()='epub:type'],' '),' toc ')]//*[local-name()='a']" \
                    -v "count(ancestor::*[local-name()='li']) - 1" -o \t -v "normalize-space(.)" -o \t -v @href -n)
                set -l f (string split \t -- $line)
                add_entry "$f[1]" "$f[2]" "$f[3]" "$base"
            end
            break
        end
    end
    if test (count $g_lab) -lt 3 -a (count $s_id) -gt 3
        set -g g_depth
        set -g g_lab
        set -g g_path
        set -g g_frag
        for p in $s_path
            test -n "$p"; or continue
            add_entry 0 (xsel $x/$p -v "normalize-space((//*[local-name()='h1' or local-name()='h2' or local-name()='h3'])[1])") $p ''
        end
    end

    # Onto the spine, in reading order; untitled entries merge into the previous.
    set -l t_lab
    set -l t_sp
    set -l t_frag
    set -l t_depth
    set -l last 0
    for j in (seq (count $g_lab))
        set -l s (contains -i -- $g_path[$j] $s_path); or continue
        test $s -ge $last; or continue
        set -l lab (tidy "$g_lab[$j]")
        test -n "$lab"; or continue
        set last $s
        set -a t_lab $lab
        set -a t_sp $s
        set -a t_frag "$g_frag[$j]"
        set -a t_depth $g_depth[$j]
    end

    # Chapter depth = shallowest non-matter entry; anything deeper is a
    # sub-section and left out.
    # Everything before a Contents entry is title-page material.
    set -l contents 0
    for j in (seq (count $t_lab))
        string match -rqi '^(table of )?contents$' -- $t_lab[$j]; and set contents $j; and break
    end
    # So is any entry repeating the book's title.
    set -l tkey (string lower -- $title | string replace -ra '[^\p{L}\p{N}]' '')
    set -l is_matter
    set -l cdepth 99
    for j in (seq (count $t_lab))
        set -l lkey (string lower -- $t_lab[$j] | string replace -ra '[^\p{L}\p{N}]' '')
        if test $j -lt $contents; or string match -rqi -- $matter_re $t_lab[$j]; or begin
                test -n "$tkey"; and string match -q -- "*$tkey*" $lkey
            end
            set -a is_matter 1
        else
            set -a is_matter 0
            test $t_depth[$j] -lt $cdepth; and set cdepth $t_depth[$j]
        end
    end

    set -g w_lab
    set -g w_sp
    set -g w_frag
    set -g w_keep
    set -g w_front
    if test (count $t_sp) -eq 0; or test $t_sp[1] -gt 1
        set -a w_lab '(front pages)'
        set -a w_sp 1
        set -a w_frag ''
        set -a w_keep 1
        set -a w_front 1
    end
    set -l n 0
    for j in (seq (count $t_lab))
        test $t_depth[$j] -le $cdepth; or continue
        set -l lab $t_lab[$j]
        set -l keep 1
        if test $is_matter[$j] = 1
            string match -rqi -- $junk_re $lab; and set keep 0
        else
            set -l r (string split -m1 \t -- (chapnum $lab))
            if test -n "$r[1]"
                set n $r[1]
            else
                set n (math $n + 1)
            end
            set -l name "$r[2]"
            test -z "$name"; and set name (subtitle $x/$s_path[$t_sp[$j]] $n)
            set lab $n
            test -n "$name"; and set lab "$n: $name"
        end
        set -a w_lab $lab
        set -a w_sp $t_sp[$j]
        set -a w_frag $t_frag[$j]
        set -a w_keep $keep
        set -a w_front 0
    end

    while true
        for i in (seq (count $w_lab))
            set -l mark '[x]'
            test $w_keep[$i] = 1; or set mark '[ ]'
            printf '%3d %s %s\n' $i $mark $w_lab[$i]
        end
        read -P 'chapters (N or N-M toggles, r N name renames, enter done) ' cmd; or exit 1
        test -z "$cmd"; and break
        if set -l rn (string match -rg '^r\s*(\d+)\s+(.+)$' -- $cmd)
            if test $rn[1] -ge 1 -a $rn[1] -le (count $w_lab)
                set w_lab[$rn[1]] (string trim -- $rn[2])
                set w_front[$rn[1]] 0
            end
            continue
        end
        for tok in (string split -n ' ' -- $cmd)
            set -l range (string match -rg '^(\d+)(?:-(\d+))?$' -- $tok); or continue
            test -n "$range[2]"; or set range[2] $range[1]
            for k in (seq $range[1] $range[2])
                test $k -ge 1 -a $k -le (count $w_keep); and set w_keep[$k] (math 1 - $w_keep[$k])
            end
        end
    end

    # A spine file goes when every entry starting in it (or, if none starts
    # there, the entry it continues) is dropped.
    set -l drop
    set -l kept_sp
    set -l owner 0
    for s in (seq (count $s_id))
        set -l any 0
        set -l keep 0
        for j in (seq (count $w_sp))
            if test $w_sp[$j] -eq $s
                set any 1
                set owner $j
                test $w_keep[$j] = 1; and set keep 1
            end
        end
        if test $any = 0
            set keep 1
            test $owner -gt 0; and set keep $w_keep[$owner]
        end
        if test $keep = 1
            set -a kept_sp $s
        else
            set -a drop $s_id[$s]
        end
    end
    if test (count $kept_sp) -eq 0
        echo '  every chapter dropped, skipped'
        return 1
    end

    # ── write: OPF edits, wg.ncx, cover ──
    set -l I "//*[local-name()='manifest']"
    set -l S "//*[local-name()='spine']"
    set -l ed -u "($M/*[local-name()='title'])[1]" -v $title
    if test -n "$(xmlstarlet sel -T -t -v "count($M/*[local-name()='creator'])" $opf | string match -v 0)"
        set -a ed -u "($M/*[local-name()='creator'])[1]" -v $author
    else
        set -a ed -s $M -t elem -n dc:creator -v $author
    end
    for id in $drop
        set -a ed -d "$S/*[local-name()='itemref'][@idref='$id']"
    end
    set -a ed -d "$I/*[local-name()='item'][@media-type='application/x-dtbncx+xml' or @id='wgncx']"
    set -a ed -s $I -t elem -n item -v '' --var it '$prev' \
        -s '$it' -t attr -n id -v wgncx -s '$it' -t attr -n href -v wg.ncx \
        -s '$it' -t attr -n media-type -v application/x-dtbncx+xml
    set -a ed -d "$S/@toc" -s $S -t attr -n toc -v wgncx
    if test -n "$newcover"
        set -l ext (path extension $newcover | string lower)
        set -l mt image/png
        string match -q -r 'jpe?g' -- $ext; and set mt image/jpeg; and set ext .jpg
        rm -f $x/{$odp}wgcover.*
        cp $newcover $x/{$odp}wgcover$ext
        set -a ed -d "$M/*[local-name()='meta'][@name='cover']" -d "$I/*[local-name()='item'][@id='wgcover']"
        set -a ed -s $I -t elem -n item -v '' --var ci '$prev' \
            -s '$ci' -t attr -n id -v wgcover -s '$ci' -t attr -n href -v wgcover$ext \
            -s '$ci' -t attr -n media-type -v $mt -s '$ci' -t attr -n properties -v cover-image
        set -a ed -s $M -t elem -n meta -v '' --var mc '$prev' \
            -s '$mc' -t attr -n name -v cover -s '$mc' -t attr -n content -v wgcover
    end
    if not xmlstarlet ed -L -N dc=http://purl.org/dc/elements/1.1/ $ed $opf
        echo '  OPF edit failed, skipped'
        return 1
    end

    # NCX src is relative to wg.ncx, which sits beside the OPF.
    set -l up ''
    test -n "$od"; and set up (string repeat -n (count (string split / -- $od)) '../')
    begin
        echo '<?xml version="1.0" encoding="UTF-8"?>'
        echo '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/><docTitle><text>'(xesc $title)'</text></docTitle><navMap>'
        set -l k 0
        for j in (seq (count $w_lab))
            test $w_keep[$j] = 1 -a $w_front[$j] = 0; or continue
            contains -- $w_sp[$j] $kept_sp; or continue
            set -l p $s_path[$w_sp[$j]]
            if test -n "$odp"; and string match -q -- "$odp*" $p
                set p (string sub -s (math (string length -- $odp) + 1) -- $p)
            else
                set p $up$p
            end
            test -n "$w_frag[$j]"; and set p "$p#$w_frag[$j]"
            set k (math $k + 1)
            echo "<navPoint id=\"p$k\" playOrder=\"$k\"><navLabel><text>"(xesc $w_lab[$j])"</text></navLabel><content src=\""(xesc $p)"\"/></navPoint>"
        end
        echo '</navMap></ncx>'
    end >$x/{$odp}wg.ncx

    set -l out $tmp/book.epub
    rm -f $out
    pushd $x
    test -f mimetype; and zip -qX0 $out mimetype
    zip -qXr1 $out . -x mimetype
    popd

    # ── convert; the original is replaced only once that worked ──
    set -l name (slug $title)
    set -l build $tmp/out
    mkdir -p $build
    rm -rf $build/$name
    cp $out $tmp/$name.epub
    pushd $build
    epub2wgb $tmp/$name.epub
    set -l ok $status
    popd
    rm -f $tmp/$name.epub
    if test $ok -ne 0; or not test -f $build/$name/book.wgb
        rm -rf $build/$name
        echo '  conversion failed, original kept'
        return 1
    end
    cp $out $epub
end

# ── main ─────────────────────────────────────────────────────────────────────

set -l epubs
set -l explicit 1
for a in $argv
    set -a epubs (path resolve $a)
end
if test (count $epubs) -eq 0
    set explicit 0
    set epubs $src/*.epub $src/*.EPUB
end

set -l done
for e in $epubs
    if test $explicit = 0; and unzip -Z1 $e 2>/dev/null | string match -rq '(^|/)wg\.ncx$'
        continue
    end
    echo
    echo "== "(path basename $e)
    process $e; and set -a done $e
end

set -l names (path basename $tmp/out/*/)
if test (count $names) -eq 0
    exit 0
end

# Staged in .incoming (dot dirs are never listed by booksync.js), then moved
# into place, so the reader never syncs a half-uploaded book.
set -l moves
for n in $names
    set -a moves "rm -rf '$remote/$n' && mv '$remote/.incoming/$n' '$remote/$n'"
end
echo
echo "uploading $(count $names) book(s) to $host"
if tar -C $tmp/out -cf - $names | ssh $host "mkdir -p $remote/.incoming && tar -C $remote/.incoming -xf - && chmod -R a+rX $remote/.incoming && "(string join ' && ' -- $moves)
    echo done
else
    echo "upload failed; the EPUBs are already reviewed, so rerun with them named:" >&2
    for e in $done
        echo "  books '$e'" >&2
    end
    exit 1
end
