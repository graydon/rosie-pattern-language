-- -*- Mode: Lua; -*-                                                                             
--
-- c0.lua    rpl compiler internals for rpl 0.0, 1.0
--
-- © Copyright IBM Corporation 2017.
-- LICENSE: MIT License (https://opensource.org/licenses/mit-license.html)
-- AUTHOR: Jamie A. Jennings

local c0 = {}

local string = require "string"
local coroutine = require "coroutine"

local lpeg = require "lpeg"

local P, V, C, S, R, Cmt, B =
   lpeg.P, lpeg.V, lpeg.C, lpeg.S, lpeg.R, lpeg.Cmt, lpeg.B

local locale = lpeg.locale()

local util = import "util"
local writer = import "writer"
local common = import "common"
local decode_match = common.decode_match
local pattern = common.pattern

local environment = import "environment"	    -- TEMPORARY
local boundary = environment.boundary
local lookup = environment.lookup
local bind = environment.bind

----------------------------------------------------------------------------------------
-- Compile-time error reporting
----------------------------------------------------------------------------------------

local function explain_invalid_charset_escape(a, source, char)
   local msg = "Compile error: invalid escape sequence in character set: \\" .. char
   msg = msg .. '\nin expression: ' .. writer.reveal_ast(a)
   coroutine.yield(false, msg)			    -- throw
end

local function explain_quantified_limitation(a, source, maybe_rule)
   assert(a, "did not get ast in explain_quantified_limitation")
   local name, errpos, text = decode_match(a)
   local line, pos, lnum = util.extract_source_line_from_pos(source, errpos)
   local rule_explanation = (maybe_rule and "in pattern "..maybe_rule.." of:") or ""
   local msg = "Compile error: pattern with quantifier can match the empty string: " ..
      rule_explanation .. "\n" .. writer.reveal_ast(a) .. "\n" ..
      string.format("At line %d:\n", lnum) ..
      string.format("%s\n", line) ..
      string.rep(" ", pos) .. "^"
   coroutine.yield(false, msg)			    -- throw
end

local function explain_repetition_error(a, source)
   assert(a, "did not get ast in explain_repetition_error")
   local name, errpos, text = decode_match(a)
   local line, pos, lnum = util.extract_source_line_from_pos(source, errpos)
   local min = tonumber(rep_args[1]) or 0
   local max = tonumber(rep_args[2])
   local msg = "Compile error: integer quantifiers must be positive, and min <= max \n" ..
      "Error is in expression: " .. writer.reveal(a) .. "\n" ..
      string.format("At line %d:\n", lnum) ..
      string.format("%s\n", line) ..
      string.rep(" ", pos-1) .. "^"
   coroutine.yield(false, msg)			    -- throw
end

local function explain_undefined_identifier(a, source)
   assert(a, "did not get ast in explain_undefined_identifier")
   local name, errpos, text = decode_match(a)
   local line, pos, lnum = util.extract_source_line_from_pos(source, errpos)
   local msg = "Compile error: reference to undefined identifier: " .. text .. "\n" ..
      string.format("At line %d:\n", lnum) ..
      string.format("%s\n", line) ..
      string.rep(" ", pos-1) .. "^"
   coroutine.yield(false, msg)				    -- throw
end

local function explain_undefined_charset(a, source)
   assert(a, "did not get ast in explain_undefined_charset")
   local _, errpos, name, subs = decode_match(a)
   local line, pos, lnum = util.extract_source_line_from_pos(source, errpos)
   local msg = "Compile error: named charset not defined: " .. name .. "\n" ..
      string.format("At line %d:\n", lnum) ..
      string.format("%s\n", line) ..
      string.rep(" ", pos-1) .. "^"
   coroutine.yield(false, msg)				    -- throw
end

local function explain_unknown_quantifier(a, source)
   assert(a, "did not get ast in explain_unknown_quantifier")
   local name, errpos, text, subs = decode_match(a)
   local line, pos, lnum = util.extract_source_line_from_pos(source, errpos)
   local q = subs[2]				    -- IS THIS RIGHT?
   local msg = "Compile error: unknown quantifier: " .. q .. "\n" ..
      string.format("At line %d:\n", lnum) ..
      string.format("%s\n", line) ..
      string.rep(" ", pos-1) .. "^"
   coroutine.yield(false, msg)				    -- throw
end

local function explain_grammar_error(a, source, message)
   assert(a, "did not get ast in explain_grammar_error")
   local name, errpos, text = decode_match(a)
   local line, pos, lnum = util.extract_source_line_from_pos(source, errpos)
   local maybe_rule = message:match("'%w'$")
   local rule_explanation = (maybe_rule and "in pattern "..maybe_rule.." of:") or ""
   local fmt = "Compile error: %s\n" .. writer.reveal_ast(a) --.. "\nAt line %d:\n%s\n" .. string.rep(" ", pos) .. "^"
   -- Full set of args: expl, lnum, line, pos
   if message:find("may be left recursive") then
      local msg = string.format(fmt, message)
      coroutine.yield(false, msg)		    -- throw
   else
      coroutine.yield(false, "unexpected error raised by lpeg: " .. tostring(message))
   end
end

----------------------------------------------------------------------------------------
-- Compile
----------------------------------------------------------------------------------------

-- Can't just run peg:match("") because a lookahead expression will return nil, even though it
-- cannot be put into a loop (because it consumes no input).
local function matches_empty(peg)
   local ok, msg = pcall(function() return peg^1 end)
   return (not ok) and msg:find("loop body may accept empty string")
end

-- Regarding debugging... a quantified exp fails as soon as:
-- e^0 == e* can never fail, because it can match the empty string.
-- e^1 == e+ fails when as soon as the initial attempt to match e fails.
-- e^-1 == e? can never fail because it can match the empty string
-- e{n,m} == (e * e ...)*e^(m-n) will fail when any of the sequence fails.

function c0.process_quantified_exp(a, gmr, source, env)
   assert(a, "did not get ast in process_quantified_exp")
   local name, pos, text, subs = decode_match(a)
   assert(name=="new_quantified_exp")
   local qpeg, min, max
   local append_boundary = true
   local exp = subs[1]
   local raw = (exp.type=="raw_exp")
   if raw then
      exp = exp.subs[1]
      append_boundary = false
   end
   local e = c0.compile_exp(exp, gmr, source, env)
   local epeg = e.peg
   -- Why did we skip this test when compiling a grammar?  Hmmm...  (Friday, March 10, 2017)
   --   if (not gmr) and matches_empty(epeg) then
   if matches_empty(epeg) then
      explain_quantified_limitation(a, source);
   end
   local q = subs[2]
   assert(q, "not getting quantifier clause in process_quantified_exp")
   local qname, qpos, qtext, qsubs = decode_match(q)
   if qname=="plus" then
      if append_boundary then qpeg=(epeg * boundary)^1
      else qpeg=epeg^1; end
      min=1; max=nil
   elseif qname=="star" then
      if append_boundary then qpeg = (epeg * (boundary * epeg)^0)^-1
      else qpeg=epeg^0; end
      min=0; max=nil
   elseif qname=="question" then
      qpeg=epeg^-1
      min=0; max=1
   elseif qname=="repetition" then
      assert(type(qsubs[1])=="table")
      assert(qsubs[1], "not getting min clause in process_quantified_exp")
      local mname, mpos, mtext = decode_match(qsubs[1])
      assert(mname=="low")
      min = tonumber(mtext) or 0
      assert(qsubs[2], "not getting max clause in process_quantified_exp")
      local mname, mpos, mtext = decode_match(qsubs[2])
      max = tonumber(mtext)
      if (min < 0) or (max and (max < 0)) or (max and (max < min)) then
	 explain_repetition_error(a, source)
      end
      if (not max) then
	 if (min == 0) then
	    -- same as star
	    if append_boundary then qpeg = (epeg * (boundary * epeg)^0)^-1
	    else qpeg=epeg^0
	    end
	 else
	    -- min > 0 due to prior checking
	    assert(min > 0)
	    if append_boundary then qpeg = (epeg * (boundary * epeg)^(min-1))
	    else qpeg = epeg^min
	    end
	 end
      else
	 -- here's where things get interesting, because we must match at least min copies of
	 -- epeg, and at most max.
	 if min==0 then
	    qpeg = ((append_boundary and (boundary * epeg)) or epeg)^(-max)
	 else
	    assert(min > 0)
	    qpeg = epeg
	    for i=1,(min-1) do
	       qpeg = qpeg * ((append_boundary and (boundary * epeg)) or epeg)
	    end -- for
	    if (min-max) < 0 then
	       qpeg = qpeg * ((append_boundary and (boundary * epeg) or epeg)^(min-max))
	    else
	       assert(min==max)
	    end
	 end -- if min==0
      end
   else						    -- switch on quantifier type
      explain_unknown_quantifier(a, source)
   end
   -- return peg being quantified, quantified peg, whether boundary was appended, quantifier name, min, max
   return e.peg, qpeg, append_boundary, qname, min, max
end

function c0.compile_new_quantified_exp(a, gmr, source, env)
   assert(a, "did not get ast in compile_cooked_quantified_exp")
   local epeg, qpeg, append_boundary, qname, min, max = c0.process_quantified_exp(a, gmr, source, env)
   return pattern.new{name=qname, peg=qpeg, ast=a, extra={epeg=epeg,
						      append_boundary=append_boundary,
						      qname=qname,
						      min=min,
						      max=max} }
end

-- rpl 1.0 parser produces literals that have the quotes in them
function c0.compile_literal0(a, gmr, source, env)
   assert(a, "did not get ast in compile_literal0")
   local name, pos, text = decode_match(a)
   assert(text:sub(1,1)=='"' and text:sub(-1,-1)=='"', "literal not in quotes: " .. text)
   local str, offense = common.unescape_string(text:sub(2,-2))
   if not str then
      explain_invalid_charset_escape(a, source, offense)
   else
      return pattern.new{name=name; peg=P(str); ast=a}
   end
end

function c0.compile_literal(a, gmr, source, env)
   assert(a, "did not get ast in compile_literal")
   local name, pos, text = decode_match(a)
   local str, offense = common.unescape_string(text)
   if not str then
      explain_invalid_charset_escape(a, source, offense)
   else
      return pattern.new{name=name; peg=P(str); ast=a}
   end
end

function c0.compile_ref(a, gmr, source, env)
   assert(a, "did not get ast in compile_ref")
   local reftype, pos, name, subs = decode_match(a)
   local packagename, localname
   if reftype=="ref" then
      localname = name
   elseif reftype=="extref" then
      local typ, pos, name = decode_match(subs[1])
      assert(typ=="packagename")
      packagename = name
      local typ, pos, name = decode_match(subs[2])
      assert(typ=="localname")
      localname = name
   else assert(false, "in compile_ref, got " .. reftyp)
   end
   local pat, msg = lookup(env, localname, packagename)
   if (not pat) then 
--      print("*** Lookup failed for: "); table.print(a) -- !@#
--      print("*** return values: ", pat, msg)
      explain_undefined_identifier(a, source);
   end -- throw
   assert(pattern.is(pat), "Did not get a pattern: "..tostring(pat))
   return pattern.new{name=name, peg=pat.peg, alias=pat.alias, ast=pat.ast, raw=pat.raw, uncap=pat.uncap}
end

function c0.compile_predicate(a, gmr, source, env)
   assert(a, "did not get ast in compile_predicate")
   local name, pos, text, subs = decode_match(a)
   local peg = c0.compile_exp(subs[2], gmr, source, env).peg
   local pred_clause = subs[1]
   local pred_name = pred_clause.type
   if pred_name=="negation" then peg = (- peg)
   elseif pred_name=="lookat" then peg = (# peg)
   else error("Internal compiler error: unknown predicate type: " .. tostring(pred_name))
   end
   return pattern.new{name=pred_name, peg=peg, ast=a}
end

-- Sequences from the parser are always binary, i.e. with 2 subs.
-- Regarding debugging: the failure of subs[1] is fatal for a.
function c0.compile_sequence(a, gmr, source, env)
   assert(a, "did not get ast in compile_sequence")
   local name, pos, text, subs = decode_match(a)
   local peg1, peg2
   peg1 = c0.compile_exp(subs[1], gmr, source, env).peg
   peg2 = c0.compile_exp(subs[2], gmr, source, env).peg
   return pattern.new{name=name, peg=peg1 * peg2, ast=a}
end
   
function c0.compile_named_charset(a, gmr, source, env)
   assert(a, "did not get ast in compile_named_charset")
   local name, pos, text, subs = decode_match(a)
   local complement
   if subs then					    -- core parser won't produce subs
      complement = (subs[1].type=="complement")
      if complement then assert(subs[2] and (subs[2].type=="name")); end
      name, pos, text, subs = decode_match((complement and subs[2]) or subs[1])
   end
   if name=="named_charset0" then
      assert(text:sub(1,2)=="[:" and text:sub(-2,-1)==":]")
      text = text:sub(3,-3)
   end
   local pat = locale[text]
   if not pat then
      explain_undefined_charset(a, source)
   end
   return pattern.new{name=name, peg=((complement and 1-pat) or pat), ast=a}
end

function c0.compile_range_charset(a, gmr, source, env)
   assert(a, "did not get ast in compile_range_charset")
   local rname, rpos, rtext, rsubs = decode_match(a)
   assert(rsubs and rsubs[1])
   local complement = (rsubs[1].type=="complement")
   if complement then
      assert(rsubs[2] and (rsubs[2].type=="character"))
      assert(rsubs[3] and (rsubs[3].type=="character"))
   else
      assert(rsubs[1].type=="character")
      assert(rsubs[2] and (rsubs[2].type=="character"))
   end
   local cname1, cpos1, ctext1 = decode_match(rsubs[(complement and 2) or 1])
   local cname2, cpos2, ctext2 = decode_match(rsubs[(complement and 3) or 2])
   local c1, offense1 = common.unescape_charlist(ctext1)
   local c2, offense2 = common.unescape_charlist(ctext2)
   if not c1 then
      explain_invalid_charset_escape(a, source, offense1)
   elseif not c2 then
      explain_invalid_charset_escape(a, source, offense2)
   else
      local peg = R(c1..c2)
      return pattern.new{name=rname,
			 peg=(complement and (1-peg)) or peg,
			 ast=a}
   end
end

function c0.compile_charlist(a, gmr, source, env)
   assert(a, "did not get ast in compile_charlist")
   local clname, clpos, cltext, clsubs = decode_match(a)
   local exps = "";
   assert((type(clsubs)=="table") and clsubs[1], "no sub-matches in charlist!")
   local complement = (clsubs[1].type=="complement")
   for i = (complement and 2) or 1, #clsubs do
      local v = clsubs[i]
      assert(v.type=="character", "did not get character sub in compile_charlist")
      local cname, cpos, ctext = decode_match(v)
      local ctext_unescaped, offense = common.unescape_charlist(ctext)
      if not ctext_unescaped then
	 explain_invalid_charset_escape(a, source, offense)
      end
      exps = exps .. ctext_unescaped
   end -- for
   return pattern.new{name=clname, peg=((complement and (1-S(exps))) or S(exps)), ast=a}
end

function c0.compile_charset(a, gmr, source, env)
   assert(a, "did not get ast in compile_charset")
   local name, pos, text, subs = decode_match(a)
   if subs[1].type=="range" then
      return c0.compile_range_charset(subs[1], gmr, source, env)
   elseif subs[1].type=="charlist" then
      return c0.compile_charlist(subs[1], gmr, source, env)
   else
      error("Internal error (compiler): Unknown charset type: " .. subs[1].type)
   end
end

-- Choice ASTs will have exactly two alternatives
-- Regarding debugging... 'a' fails only if both alternatives fail
function c0.compile_choice(a, gmr, source, env)
   assert(a, "did not get ast in compile_choice")
   local name, pos, text, subs = decode_match(a)
   local peg1 = c0.compile_exp(subs[1], gmr, source, env).peg
   local peg2 = c0.compile_exp(subs[2], gmr, source, env).peg
   return pattern.new{name=name, peg=(peg1+peg2), ast=a}
end

function c0.compile_raw_exp(a, gmr, source, env)
   assert(a, "did not get ast in compile_raw_exp")
   local name, pos, text, subs = decode_match(a)
   assert(name=="raw_exp")
   assert(not subs[2])
   local pat = c0.compile_exp(subs[1], gmr, source, env)
   return pattern.new{name=name, peg=pat.peg, ast=pat.ast}
end

function c0.compile_syntax_error(a, gmr, source, env)
   assert(a, "did not get ast in compile_syntax_error")
   error("Compiler called on source code with errors! " .. writer.reveal_exp(a))
end

function c0.compile_grammar_expression(a, gmr, source, env)
   assert(a, "did not get ast in compile_grammar_expression")
   local name, pos, text, subs = decode_match(a)
   assert(name=="grammar_" or name=="new_grammar" or name=="grammar_expression")
   assert(type(subs[1])=="table")
   local gtable = environment.extend(env)
   local first = subs[1]			    -- first rule in grammar
   assert(first, "not getting first rule in compile_grammar_expression")
   local fname, fpos, ftext = decode_match(first)
   assert(fname=="binding")

   local rule, id_node, id, exp_node
   
   -- first pass: collect rule names as V() refs into a new env
   for i = 1, #subs do			    -- for each rule
      local rule = subs[i]
      assert(rule, "not getting rule in compile_grammar_expression")
      local rname, rpos, rtext, rsubs = decode_match(rule)
      assert(rname=="binding")
      local id_node = rsubs[1]			    -- identifier clause
      assert(id_node and (id_node.type=="identifier" or id_node.type=="localname"),
	     "grammar rule lhs not an identifier or localname: " .. id_node.type)
      local iname, ipos, id = decode_match(id_node)
      local exp_node = rsubs[2]
      assert(exp_node)
      local alias_flag = not rule.capture
      bind(gtable,id,pattern.new{name=id, peg=V(id), alias=alias_flag})
   end						    -- for

   -- second pass: compile right hand sides in gtable environment
   local pats = {}
   local start
   for i = 1, #subs do			    -- for each rule
      rule = subs[i]
      assert(rule, "not getting rule in compile_grammar_expression")
      local rname, rpos, rtext, rsubs = decode_match(rule)
      id_node = rsubs[1]			    -- identifier clause
      assert(id_node, "not getting id_node in compile_grammar_expression")
      local iname, ipos, id, isubs = decode_match(id_node)
      if not start then start=id; end		    -- first rule is start rule
      exp_node = rsubs[2]			    -- expression clause
      assert(exp_node, "not getting exp_node in compile_grammar_expression")
      pats[id] = c0.compile_exp(exp_node, true, source, gtable) -- gmr flag is true 
   end -- for

   -- third pass: create the table that will create the LPEG grammar 
   local t = {}
   for id, pat in pairs(pats) do t[id] = pat.peg; end
   t[1] = start					    -- first rule is start rule
   local success, peg_or_msg = pcall(P, t)	    -- P(t) while catching errors
   if success then
      return pattern.new{name="grammar", peg=peg_or_msg, ast=a, alias=lookup(gtable,t[1]).alias}, start
   else -- failed
      assert(type(peg_or_msg)=="string", "Internal error (compiler) while reporting an error in a grammar")
      explain_grammar_error(a, source, peg_or_msg)
   end
end

function c0.compile_grammar(a, gmr, source, env)
   local pat, name = c0.compile_grammar_expression(a, gmr, source, env)
   -- if no pattern returned, then errors were already explained
   if pat then
      local msg
      if lookup(env,name) then msg = "Warning: reassignment to identifier " .. name; end
      bind(env,name,pat)
      return pat, msg
   else
      -- should never get here.  when compile_grammar_expression fails, it throws.
      coroutine.yield(false, "compilation of grammar failed -- no additional information is available")
   end
end

function c0.expression_p(ast)
   local name, pos, text, subs = decode_match(ast)
   return not (not c0.compile_exp_functions[name])
end

function c0.compile_capture(a, gmr, source, env)
   assert(a, "did not get ast in compile_capture")
   local name, pos, text, subs = decode_match(a)
   assert(name=="capture")
   assert(subs and subs[1] and subs[2] and (not subs[3]), "wrong number of subs in capture ast")
   local ref_exp, captured_exp = subs[1], subs[2]
   local cap_name, cap_pos, cap_text, cap_subs = decode_match(captured_exp)
   local pat

   assert(c0.expression_p(captured_exp),
	  "compile_capture called with an ast that is not an expression: " .. captured_exp.type)

   local refname, _, reftext, _ = decode_match(ref_exp)
   assert(refname=="ref")
   assert(type(reftext)=="string")

   pat = c0.compile_exp(captured_exp, gmr, source, env)
   pat.name = cap_name
   if pat.uncap then
      -- In this case, we are capturing a reference that is itself a capture.  So what we want to
      -- do is a re-capture, i.e. ignore the existing capture.
      pat.peg = common.match_node_wrap(pat.uncap, reftext)
   else
      pat.uncap = pat.peg
      pat.peg = common.match_node_wrap(pat.peg, reftext)
   end
   return pat
end

c0.compile_exp_functions = {"compile_exp";
				    capture=c0.compile_capture;	    
				    ref=c0.compile_ref;
			            extref=c0.compile_ref;
				    predicate=c0.compile_predicate;
				    raw_exp=c0.compile_raw_exp;
				    choice=c0.compile_choice;
				    sequence=c0.compile_sequence;
				    literal=c0.compile_literal;
				    literal0=c0.compile_literal0;
				    named_charset=c0.compile_named_charset;
				    named_charset0=c0.compile_named_charset;
				    range=c0.compile_range_charset;
				    charlist=c0.compile_charlist;
				    charset=c0.compile_charset;        -- ONLY USED IN CORE
				    new_quantified_exp=c0.compile_new_quantified_exp;
				    syntax_error=c0.compile_syntax_error;
				    grammar_expression=c0.compile_grammar_expression;
				 }

function c0.compile_exp(a, gmr, source, env)
   return common.walk_ast(a, c0.compile_exp_functions, gmr, source, env)
end

local function compile_rhs(a, gmr, source, env, iname)
   assert(type(a)=="table", "did not get ast in compile_rhs: " .. tostring(a))
   if not c0.expression_p(a) then
      local msg = string.format('Compile error: expected an expression, but received %q',
				writer.reveal_ast(a))
      error(msg)
   end
   local pat = c0.compile_exp(a, gmr, source, env)
   local rhs_name = a.type
   pat.raw = ((rhs_name=="raw_exp") or
	      ((rhs_name=="capture") and (a.subs[2].type=="ref") and pat.raw) or
	      ((rhs_name=="ref") and pat.raw))
   pat.ast = a;
   return pat
end

function c0.compile_binding(a, gmr, source, env)
   assert(a, "did not get ast in compile_binding")
   local name, pos, text, subs = decode_match(a)
   local lhs, rhs = subs[1], subs[2]
   assert(lhs.type=="identifier" or lhs.type=="localname", "in c0.compile_binding, got: " .. tostring(lhs.type))
   assert(type(rhs)=="table")			    -- the right side of the assignment
   assert(not subs[3])
   assert(type(source)=="string")
   assert(a and (type(a.capture)=="boolean"))
   local _, ipos, iname = decode_match(lhs)
   local pat = compile_rhs(rhs, gmr, source, env, iname)
   pat.alias = (not a.capture)
   local msg
   if lookup(env,iname) then msg = "Warning: reassignment to identifier " .. iname; end
   bind(env,iname,pat)
   return pat, msg
end

function c0.compile_ast(ast, source, env)
   assert(type(ast)=="table", "Compiler: first argument not an ast: "..tostring(ast))
   local functions = {"compile_ast";
		      binding=c0.compile_binding;
		      new_grammar=c0.compile_grammar;
		      exp=c0.compile_exp;
		      default=c0.compile_exp;
		   }
   return common.walk_ast(ast, functions, false, source, env)
end

return c0
