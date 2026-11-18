using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;

namespace XLuaFastCallEditor
{
    // Small declaration lexer, not an expression evaluator. Defaults are skipped as
    // balanced token groups; their values always come from the C# compiler.
    public static class XLuaFastCallAutoTags
    {
        sealed class Token { public string Text; public int Start, End; }
        sealed class Member
        {
            public int Start, Open, Close;
            public string Name, Result;
            public bool PublicInstance;
            public readonly List<Tuple<int,int>> Attributes = new List<Tuple<int,int>>();
        }
        sealed class Edit { public int Start, Length; public string Text; }

        public static string RebuildSource(Type targetType, string source, string conditionSymbol, out MethodInfo[] selected)
        {
            if(targetType==null || !targetType.IsClass || !targetType.IsVisible || targetType.ContainsGenericParameters)
                throw new ArgumentException("Auto tags require a visible non-generic class",nameof(targetType));
            if(source==null)throw new ArgumentNullException(nameof(source));
            if(!string.IsNullOrEmpty(conditionSymbol) && !Regex.IsMatch(conditionSymbol,@"^[A-Za-z_][A-Za-z0-9_]*$"))
                throw new ArgumentException("ConditionSymbol must be a single C# symbol");
            var tokens=Lex(source);
            var pairs=Pairs(tokens);
            var bodies=new List<Tuple<int,int>>();
            FindType(tokens,pairs,0,tokens.Count,"",targetType.FullName.Replace('+','.'),bodies);
            if(bodies.Count!=1)throw new InvalidOperationException("Expected exactly one declaration of "+targetType.FullName+" in configured source; found "+bodies.Count);
            var members=Members(tokens,pairs,bodies[0].Item1,bodies[0].Item2);
            var methods=targetType.GetMethods(BindingFlags.Public|BindingFlags.Instance|BindingFlags.DeclaredOnly)
                .Where(m=>XLuaFastCallGenerator.UnsupportedReason(m)==null).ToArray();
            var candidates=new List<Tuple<Member,MethodInfo>>();
            foreach(var member in members)
                foreach(var method in methods)
                    if(Matches(tokens,pairs,member,method))candidates.Add(Tuple.Create(member,method));
            var ambiguous=new HashSet<MethodInfo>(candidates.GroupBy(c=>c.Item2).Where(g=>g.Count()!=1).Select(g=>g.Key));
            var accepted=candidates.Where(c=>!ambiguous.Contains(c.Item2)).ToArray();
            selected=accepted.Select(c=>c.Item2).ToArray();

            var edits=new List<Edit>();
            foreach(var member in members)
                foreach(var attribute in member.Attributes)RemoveTag(source,tokens,pairs,attribute,edits);
            string clean=Apply(source,edits);
            // Reparse after removing attributes so insertion offsets remain correct.
            tokens=Lex(clean);pairs=Pairs(tokens);bodies.Clear();
            FindType(tokens,pairs,0,tokens.Count,"",targetType.FullName.Replace('+','.'),bodies);
            members=Members(tokens,pairs,bodies[0].Item1,bodies[0].Item2);
            string newline=source.Contains("\r\n")?"\r\n":"\n";
            edits.Clear();
            foreach(var member in members)
            {
                if(!selected.Any(m=>Matches(tokens,pairs,member,m)))continue;
                int start=tokens[member.Start].Start;
                int line=clean.LastIndexOf('\n',Math.Max(0,start-1))+1;
                string prefix=clean.Substring(line,start-line);
                bool ownLine=prefix.All(c=>c==' ' || c=='\t');
                string indent=ownLine?prefix:"";
                string tag=indent+"[XLuaFastCall]"+newline;
                if(!string.IsNullOrEmpty(conditionSymbol))tag="#if "+conditionSymbol+newline+tag+"#endif"+newline;
                edits.Add(new Edit {Start=ownLine?line:start,Text=(ownLine?"":newline)+tag});
            }
            string result=Apply(clean,edits);
            return selected.Length==0?result:EnsureImport(result,conditionSymbol,newline);
        }

        // Only file-level imports count. A conditional import is reusable when
        // its simple #if conditions match the configured tag condition.
        static string EnsureImport(string source,string conditionSymbol,string newline)
        {
            var tokens=Lex(source,true);
            var conditions=new List<bool>();
            int insertion=0;bool prefix=true;
            for(int i=0;i<tokens.Count;i++) {
                var token=tokens[i];string word=token.Text;
                if(word.StartsWith("#",StringComparison.Ordinal)) {
                    var directive=Regex.Match(word,@"^#\s*(\w+)\s*(.*)$");
                    string command=directive.Groups[1].Value;
                    string expression=directive.Groups[2].Value.Trim();
                    if(command=="if")conditions.Add(expression=="true" || (!string.IsNullOrEmpty(conditionSymbol) && expression==conditionSymbol));
                    else if((command=="else" || command=="elif") && conditions.Count>0)conditions[conditions.Count-1]=false;
                    else if(command=="endif" && conditions.Count>0)conditions.RemoveAt(conditions.Count-1);
                    if(prefix && conditions.Count==0)insertion=token.End;
                    continue;
                }
                if(word=="extern" && i+1<tokens.Count && tokens[i+1].Text=="alias") {
                    while(i<tokens.Count && tokens[i].Text!=";")i++;
                    if(i<tokens.Count && conditions.Count==0)insertion=tokens[i].End;
                    continue;
                }
                prefix=false;
                bool globalImport=word=="global" && i+1<tokens.Count && tokens[i+1].Text=="using";
                if(word=="using" || globalImport) {
                    if(globalImport)i++;
                    int end=i+1;while(end<tokens.Count && tokens[end].Text!=";")end++;
                    string import=Join(tokens,i+1,end);
                    if((import=="XLua" || import=="global::XLua") && conditions.All(c=>c))return source;
                    if(globalImport && conditions.Count==0 && end<tokens.Count)insertion=tokens[end].End;
                    i=end;
                }
                else break; // namespace, attributes or type declaration
            }
            string text="using XLua;"+newline;
            if(!string.IsNullOrEmpty(conditionSymbol))text="#if "+conditionSymbol+newline+text+"#endif"+newline;
            // A directive's End excludes its newline; preserve #define/extern order.
            if(insertion>0) {
                if(insertion<source.Length && source[insertion]=='\r')insertion++;
                if(insertion<source.Length && source[insertion]=='\n')insertion++;
                else text=newline+text;
            }
            return source.Insert(insertion,text);
        }

        static List<Token> Lex(string source,bool includeDirectives=false)
        {
            var result=new List<Token>();
            for(int i=0;i<source.Length;)
            {
                if(char.IsWhiteSpace(source[i])){i++;continue;}
                if(source[i]=='#') {
                    int directiveStart=i;while(i<source.Length && source[i]!='\n')i++;
                    if(includeDirectives)result.Add(new Token {Text=source.Substring(directiveStart,i-directiveStart).TrimEnd('\r'),Start=directiveStart,End=i});
                    continue;
                }
                if(i+1<source.Length && source[i]=='/' && source[i+1]=='/')
                {while(i<source.Length && source[i]!='\n')i++;continue;}
                if(i+1<source.Length && source[i]=='/' && source[i+1]=='*')
                {int end=source.IndexOf("*/",i+2,StringComparison.Ordinal);if(end<0)throw new InvalidOperationException("Unclosed comment");i=end+2;continue;}
                int start=i;
                int quoteAt=i;
                while(quoteAt<source.Length && (source[quoteAt]=='@' || source[quoteAt]=='$'))quoteAt++;
                if(source[i]=='\'' || (quoteAt<source.Length && source[quoteAt]=='\"'))
                {
                    i=SkipLiteral(source,i,quoteAt);
                    result.Add(new Token {Text="<literal>",Start=start,End=i});continue;
                }
                if(char.IsLetter(source[i]) || source[i]=='_' || source[i]=='@')
                {i++;while(i<source.Length && (char.IsLetterOrDigit(source[i]) || source[i]=='_'))i++;}
                else i++;
                result.Add(new Token {Text=source.Substring(start,i-start),Start=start,End=i});
            }
            return result;
        }

        static int SkipLiteral(string source,int start,int quoteAt)
        {
            bool verbatim=source.Substring(start,quoteAt-start).Contains("@");
            bool interpolated=source.Substring(start,quoteAt-start).Contains("$");
            char quote=source[quoteAt];int i=quoteAt+1;
            while(i<source.Length)
            {
                if(!verbatim && source[i]=='\\'){i=Math.Min(i+2,source.Length);continue;}
                if(interpolated && source[i]=='{')
                {
                    if(i+1<source.Length && source[i+1]=='{'){i+=2;continue;}
                    int depth=1;i++;
                    while(i<source.Length && depth>0)
                    {
                        int q=i;while(q<source.Length && (source[q]=='@' || source[q]=='$'))q++;
                        if(source[i]=='\'' || (q<source.Length && source[q]=='\"')){i=SkipLiteral(source,i,q);continue;}
                        if(source[i]=='{')depth++;
                        else if(source[i]=='}')depth--;
                        i++;
                    }
                    if(depth!=0)throw new InvalidOperationException("Unclosed string interpolation");
                    continue;
                }
                if(source[i++]!=quote)continue;
                if(verbatim && i<source.Length && source[i]==quote){i++;continue;}
                return i;
            }
            throw new InvalidOperationException("Unclosed string or character literal");
        }

        static Dictionary<int,int> Pairs(List<Token> t)
        {
            var pairs=new Dictionary<int,int>();var stack=new Stack<int>();
            for(int i=0;i<t.Count;i++)
            {
                string s=t[i].Text;
                if(s=="(" || s=="[" || s=="{")stack.Push(i);
                else if(s==")" || s=="]" || s=="}")
                {
                    if(stack.Count==0)throw new InvalidOperationException("Unbalanced source delimiters");
                    int open=stack.Pop();
                    if((t[open].Text=="("? ")":t[open].Text=="["?"]":"}")!=s)throw new InvalidOperationException("Unbalanced source delimiters");
                    pairs[open]=i;
                }
            }
            if(stack.Count!=0)throw new InvalidOperationException("Unbalanced source delimiters (including conditional branches)");
            return pairs;
        }

        static void FindType(List<Token> t,Dictionary<int,int> pairs,int begin,int end,string scope,string target,List<Tuple<int,int>> bodies)
        {
            for(int i=begin;i<end;i++)
            {
                string word=t[i].Text;
                if(word=="namespace")
                {
                    int j=i+1;while(j<end && t[j].Text!="{" && t[j].Text!=";")j++;
                    string ns=scope+Join(t,i+1,j)+".";
                    if(j<end && t[j].Text=="{"){FindType(t,pairs,j+1,pairs[j],ns,target,bodies);i=pairs[j];}
                    else {scope=ns;i=j;}
                }
                else if((word=="class" || word=="struct" || word=="interface" || word=="enum") && i+1<end)
                {
                    string name=scope+t[i+1].Text.TrimStart('@');int j=i+2;
                    while(j<end && t[j].Text!="{" && t[j].Text!=";")j++;
                    if(j==end || t[j].Text!="{")continue;
                    if(name==target)bodies.Add(Tuple.Create(j+1,pairs[j]));
                    FindType(t,pairs,j+1,pairs[j],name+".",target,bodies);i=pairs[j];
                }
                else if(pairs.ContainsKey(i))i=pairs[i];
            }
        }

        static List<Member> Members(List<Token> t,Dictionary<int,int> pairs,int begin,int end)
        {
            var result=new List<Member>();
            for(int i=begin;i<end;)
            {
                var m=new Member {Start=i};int j=i;
                while(j<end && t[j].Text=="["){m.Attributes.Add(Tuple.Create(j,pairs[j]));j=pairs[j]+1;}
                int declaration=j;
                while(j<end && t[j].Text!="(" && t[j].Text!="{" && t[j].Text!=";" && t[j].Text!="=")j++;
                if(j<end && t[j].Text=="(" && j>declaration)
                {
                    m.Open=j;m.Close=pairs[j];m.Name=t[j-1].Text.TrimStart('@');
                    int r=declaration;
                    while(r<j-1 && new[]{"public","private","protected","internal","static","virtual","override","sealed","new","unsafe","async","extern","partial"}.Contains(t[r].Text))r++;
                    var modifiers=t.Skip(declaration).Take(r-declaration).Select(x=>x.Text).ToArray();
                    m.PublicInstance=modifiers.Contains("public") && !modifiers.Contains("static");
                    m.Result=Join(t,r,j-1);result.Add(m);j=m.Close+1;
                }
                // Skip implementation, including nested types/lambdas/property accessors.
                while(j<end && t[j].Text!=";" && t[j].Text!="{")
                {if(pairs.ContainsKey(j))j=pairs[j];j++;}
                if(j<end && t[j].Text=="{")j=pairs[j];
                i=j+1;
            }
            return result;
        }

        static bool Matches(List<Token> t,Dictionary<int,int> pairs,Member member,MethodInfo method)
        {
            if(!member.PublicInstance || member.Name!=method.Name || !TypeMatches(member.Result,method.ReturnType))return false;
            var segments=Segments(t,pairs,member.Open+1,member.Close);var ps=method.GetParameters();
            if(segments.Count!=ps.Length)return false;
            for(int i=0;i<ps.Length;i++)
            {
                int a=segments[i].Item1,b=segments[i].Item2;
                while(a<b && t[a].Text=="[")a=pairs[a]+1;
                string modifier="";
                if(a<b && new[]{"ref","out","in","params","this"}.Contains(t[a].Text))modifier=t[a++].Text;
                int equals=a;
                while(equals<b && t[equals].Text!="="){if(pairs.ContainsKey(equals))equals=pairs[equals];equals++;}
                b=equals;
                if(b-a<2 || t[b-1].Text.TrimStart('@')!=ps[i].Name)return false;
                string expected=ps[i].IsOut?"out":ps[i].ParameterType.IsByRef?(ps[i].IsIn?"in":"ref"):"";
                if(modifier!=expected || !TypeMatches(Join(t,a,b-1),ps[i].ParameterType.IsByRef?ps[i].ParameterType.GetElementType():ps[i].ParameterType))return false;
            }
            return true;
        }

        static List<Tuple<int,int>> Segments(List<Token> t,Dictionary<int,int> pairs,int begin,int end)
        {
            var result=new List<Tuple<int,int>>();int start=begin;
            for(int i=begin;i<end;i++)
            {
                if(pairs.ContainsKey(i)){i=pairs[i];continue;}
                if(t[i].Text==","){result.Add(Tuple.Create(start,i));start=i+1;}
            }
            if(start<end)result.Add(Tuple.Create(start,end));return result;
        }
        static string Join(List<Token> t,int a,int b)=>string.Concat(t.Skip(a).Take(b-a).Select(x=>x.Text));
        static bool TypeMatches(string text,Type type)
        {
            string alias=type==typeof(void)?"void":type==typeof(int)?"int":type==typeof(byte)?"byte":type==typeof(long)?"long":type==typeof(float)?"float":type==typeof(double)?"double":type==typeof(bool)?"bool":type==typeof(string)?"string":null;
            text=text.Replace("global::","");return alias!=null && (text==alias || text==type.Name || text==type.FullName);
        }
        static void RemoveTag(string source,List<Token> t,Dictionary<int,int> pairs,Tuple<int,int> attribute,List<Edit> edits)
        {
            var segments=Segments(t,pairs,attribute.Item1+1,attribute.Item2);
            var tags=segments.Where(s=>Regex.IsMatch(Join(t,s.Item1,s.Item2),@"^(?:global::)?(?:XLua\.)?XLuaFastCall(?:Attribute)?(?:\(\))?$" )).ToArray();
            if(tags.Length==0)return;
            if(tags.Length==segments.Count)
            {
                int a=t[attribute.Item1].Start,b=t[attribute.Item2].End;
                var blocks=Regex.Matches(source,@"(?m)^[ \t]*#if[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*\r?\n[ \t]*\[(?:global::)?(?:XLua\.)?XLuaFastCall(?:Attribute)?(?:\(\))?\][ \t]*\r?\n[ \t]*#endif[ \t]*(?:\r?\n|$)");
                var block=blocks.Cast<Match>().FirstOrDefault(m=>m.Index<=a && m.Index+m.Length>=b);
                if(block!=null){a=block.Index;b=block.Index+block.Length;}
                else {
                    int line=source.LastIndexOf('\n',Math.Max(0,a-1))+1;
                    int next=source.IndexOf('\n',b);if(next<0)next=source.Length-1;
                    if(source.Substring(line,a-line).Trim().Length==0 && source.Substring(b,Math.Max(0,next-b)).Trim().Length==0){a=line;b=next+1;}
                }
                edits.Add(new Edit {Start=a,Length=b-a,Text=""});
            }
            else
            {
                // Preserve other attributes and their arguments verbatim.
                var kept=segments.Except(tags).Select(s=>source.Substring(t[s.Item1].Start,t[s.Item2-1].End-t[s.Item1].Start));
                int a=t[attribute.Item1].Start,b=t[attribute.Item2].End;
                edits.Add(new Edit {Start=a,Length=b-a,Text="["+string.Join(", ",kept)+"]"});
            }
        }
        static string Apply(string source,List<Edit> edits)
        {
            int boundary=source.Length;
            foreach(var edit in edits.OrderByDescending(e=>e.Start))
            {
                if(edit.Start+edit.Length>boundary)throw new InvalidOperationException("Overlapping source edits");
                source=source.Remove(edit.Start,edit.Length).Insert(edit.Start,edit.Text);boundary=edit.Start;
            }
            return source;
        }
    }
}
