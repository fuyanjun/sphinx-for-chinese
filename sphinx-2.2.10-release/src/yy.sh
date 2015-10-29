bison -l -d -o yysphinxexpr.c sphinxexpr.y
bison -l -d -o yysphinxselect.c sphinxselect.y
bison -l -d -o yysphinxquery.c sphinxquery.y
bison -l -d -o yysphinxql.c sphinxql.y
bison -l -d -o yysphinxjson.c sphinxjson.y
flex -i -ollsphinxql.c sphinxql.l
flex -i -ollsphinxjson.c -Pyy2 sphinxjson.l

perl -npe "s/  __attr/\/\/  __attr/" -i.bak yysphinxexpr.c
perl -npe "s/^yyerrlab1:/\/\/yyerrlab1:/m;s/  __attr/\/\/  __attr/" -i.bak yysphinxselect.c
perl -npe "s/^yyerrlab1:/\/\/yyerrlab1:/m;s/  __attr/\/\/  __attr/" -i.bak yysphinxquery.c
perl -npe "s/  __attr/\/\/  __attr/" -i.bak yysphinxjson.c
perl -npe "s/(#include <unistd.h>)/#if !USE_WINDOWS\n\1\n#endif/;s/\(size_t\) num_to_read/num_to_read/" -i.bak llsphinxql.c
perl -npe "s/\(size_t\) num_to_read/num_to_read/" -i.bak llsphinxjson.c
perl -npe "s/size_t n; \\\\/int n; \\\\/" -i.bak llsphinxql.c
perl -npe "s/size_t n; \\\\/int n; \\\\/" -i.bak llsphinxjson.c

patch -s -p0 -i yysphinxql.patch

# fix buffer overflows in generated files
perl -npe "s/if \(yycheck/if \(yyx\+yyn<int\(sizeof\(yycheck\)\/sizeof\(yycheck\[0\]\)\) && yycheck/" -i.bak yysphinxexpr.c
perl -npe "s/if \(yycheck/if \(yyx\+yyn<int\(sizeof\(yycheck\)\/sizeof\(yycheck\[0\]\)\) && yycheck/" -i.bak yysphinxselect.c
perl -npe "s/if \(yycheck/if \(yyx\+yyn<int\(sizeof\(yycheck\)\/sizeof\(yycheck\[0\]\)\) && yycheck/" -i.bak yysphinxquery.c
perl -npe "s/if \(yycheck/if \(yyx\+yyn<int\(sizeof\(yycheck\)\/sizeof\(yycheck\[0\]\)\) && yycheck/" -i.bak yysphinxql.c
perl -npe "s/if \(yycheck/if \(yyx\+yyn<int\(sizeof\(yycheck\)\/sizeof\(yycheck\[0\]\)\) && yycheck/" -i.bak yysphinxjson.c

rm *.bak 2>/dev/null
rm yysphinxql.c.orig 2>/dev/null
