---- -*- Mode: Lua; -*-                                                                           
----
---- cli-test.lua      sniff test for the CLI
----
---- © Copyright IBM Corporation 2016, 2017.
---- LICENSE: MIT License (https://opensource.org/licenses/mit-license.html)
---- AUTHOR: Jamie A. Jennings

assert(TEST_HOME, "TEST_HOME is not set")

test.start(test.current_filename())

lpeg = import "lpeg"
list = import "list"
util = import "util"
violation = import "violation"
check = test.check

rosie_cmd = ROSIE_HOME .. "/bin/rosie"
local try = io.open(rosie_cmd, "r")
if try then
   try:close()					    -- found it.  will use it.
else
   local tbl, status, code = util.os_execute_capture("command -v rosie")
   if code==0 and tbl and tbl[1] and type(tbl[1])=="string" then
      rosie_cmd = tbl[1]:sub(1,-2)			    -- remove lf at end
   else
      error("Cannot find rosie executable")
   end
end
print("Found rosie executable: " .. rosie_cmd)

function check_lua_error(results_as_string)
   check(not results_as_string:find("traceback"), "lua error???", 1)
   check(not results_as_string:find("src/core"), "lua error???", 1)
end


infilename = TEST_HOME .. "/resolv.conf"

-- N.B. grep_flag does double duty:
-- false  ==> use the match command
-- true   ==> use the grep command
-- string ==> use the grep command and add this string to the command (e.g. to set the output encoder)
function run(import, expression, grep_flag, expectations)
   test.heading(expression)
   test.subheading((grep_flag and "Using grep command") or "Using match command")
   local verb = (grep_flag and "Grepping for") or "Matching"
   local import_option = ""
   if import then import_option = " --rpl '" .. import .. "' "; end
   local grep_extra_options = type(grep_flag)=="string" and (" " .. grep_flag .. " ") or ""
   local cmd = rosie_cmd .. import_option ..
      (grep_flag and " grep" or " match") .. grep_extra_options ..
      " '" .. expression .. "' " .. infilename
   cmd = cmd .. " 2>&1"
   local results, status, code = util.os_execute_capture(cmd, nil, "l")
   if not results then
      print(cmd)
      print("\nTesting " .. verb .. " '" .. expression .. "' against fixed input ")
      error("Run failed: " .. tostring(status) .. ", " .. tostring(code)); end
   local mismatch_flag = false;
   if expectations then
      if results[1]=="Loading rosie from source" then
	 table.remove(results, 1)
      end
      for i=1, #expectations do 
	 if expectations then
	    if results[i]~=expectations[i] then
	       print(results[i])
	       print("Mismatch")
	       mismatch_flag = true
	    end
	 end
      end -- for
      if mismatch_flag then
	 print(cmd)
	 io.write("\nTesting " .. verb .. " '" .. expression .. "' against fixed input: ")
	 print("SOME MISMATCHED OUTPUT WAS FOUND.");
      end
      if (not (#results==#expectations)) then
	 print(cmd)
	 io.write("\nTesting " .. verb .. " '" .. expression .. "' against fixed input: ")
	 print(string.format("Received %d results, expected %d", #results, #expectations))
      end
      check((not mismatch_flag), "Mismatched output compared to expectations", 1)
      check((#results==#expectations), "Mismatched number of results compared to expectations", 1)
   end -- if expectations
   return results
end




---------------------------------------------------------------------------------------------------
test.heading("Match and grep commands")
---------------------------------------------------------------------------------------------------

results_all_things = 
   { "[39;1m#[0m",
     "[39;1m#[0m [33mThis[0m [33mis[0m [33man[0m [33mexample[0m [33mfile[0m[39;1m,[0m [33mhand[0m[39;1m-[0m[33mgenerated[0m [33mfor[0m [33mtesting[0m [33mrosie[0m[39;1m.[0m",
     "[39;1m#[0m [33mLast[0m [33mupdate[0m[39;1m:[0m [34mWed[0m [34mJun[0m [34m28[0m [1;34m16[0m:[1;34m58[0m:[1;34m22[0m [1;34mEDT[0m [34m2017[0m",
     "[39;1m#[0m ",
     "[33mdomain[0m [31mabc.aus.example.com[0m",
     "[33msearch[0m [31mibm.com[0m [31mmylocaldomain.myisp.net[0m [31mexample.com[0m",
     "[33mnameserver[0m [31m192.9.201.1[0m",
     "[33mnameserver[0m [31m192.9.201.2[0m",
     "[33mnameserver[0m [31;4mfde9:4789:96dd:03bd::1[0m",
   }

results_common_word =
   {"[33mdomain[0m abc.aus.example.com",
    "[33msearch[0m ibm.com mylocaldomain.myisp.net example.com",
    "[33mnameserver[0m 192.9.201.1",
    "[33mnameserver[0m 192.9.201.2",
    "[33mnameserver[0m fde9:4789:96dd:03bd::1"
 }

results_common_word_grep = 
   {"# This is an example file, hand-generated for testing rosie.",
    "# Last update: Wed Jun 28 16:58:22 EDT 2017",
    "domain abc.aus.example.com",
    "search ibm.com mylocaldomain.myisp.net example.com",
    "nameserver 192.9.201.1",
    "nameserver 192.9.201.2",
    "nameserver fde9:4789:96dd:03bd::1",
    }

results_common_word_grep_matches_only = 
   {"This",
    "is",
    "an",
    "example",
    "file",
    "hand",
    "generated",
    "for",
    "testing",
    "rosie",
    "Last",
    "update",
    "Wed",
    "Jun",
    "EDT",
    "domain",
    "abc",
    "aus",
    "example",
    "com",
    "search",
    "ibm",
    "com",
    "mylocaldomain",
    "myisp",
    "net",
    "example",
    "com",
    "nameserver",
    "nameserver",
    "nameserver",
    }

results_word_network = 
   {"[33mdomain[0m [31mabc.aus.example.com[0m",
    "[33msearch[0m [31mibm.com[0m mylocaldomain.myisp.net example.com",
    "[33mnameserver[0m [31m192.9.201.1[0m",
    "[33mnameserver[0m [31m192.9.201.2[0m",
    "[33mnameserver[0m [31;4mfde9:4789:96dd:03bd::1[0m"
 }

results_number_grep =
   { " 28 ",
     "16",
     "58",
     "22 ",
     " 2017",
     " abc",
     " 192.9",
     "201.1",
     " 192.9",
     "201.2",
     " fde9",
     "4789",
     "96dd",
     "03bd",
     "1",
  }

run(false, "all.things", false, results_all_things)

run("import word", "word.any", false, results_common_word)
run("import word", "word.any", true, results_common_word_grep)
run("import word, net", "word.any net.any", false, results_word_network)
run("import num", "~ num.any ~", "-o subs", results_number_grep)

ok, msg = pcall(run, "import word", "foo = word.any", nil, nil)
check(ok)
check(table.concat(msg, "\n"):find("Syntax error"))

ok, msg = pcall(run, "import word", "/foo/", nil, nil)
check(ok)
check(table.concat(msg, "\n"):find("Syntax error"))

ok, ignore = pcall(run, "import word", '"Gold"', nil, nil)
check(ok, [[testing for a shell quoting error in which rpl expressions containing double quotes
      were not properly passed to lua in bin/run-rosie]])

cmd = rosie_cmd .. " list --rpl 'lua_ident = {[[:alpha:]] / \"_\" / \".\" / \":\"}+' 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "Expression on command line can contain [[.,.]]") -- command succeeded
check(code==0, "Return code is zero")
results_txt = table.concat(results, '\n')
check(results_txt:find("lua_ident"))
check(results_txt:find("names"))
if (#results <=0) or (code ~= 0) then
   print(cmd)
   print("\nChecking that the command line expression can contain [[...]] per Issue #22")
end

---------------------------------------------------------------------------------------------------
test.heading("Test command")

-- Passing tests
cmd = rosie_cmd .. " test " .. TEST_HOME .. "/lightweight-test-pass.rpl 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0)
check(code==0, "Return code is zero")
check(results[#results]:find("tests passed"))
if (#results <=0) or (code ~= 0) then
   print(cmd)
   print("\nSniff test of the lightweight test facility (MORE TESTS LIKE THIS ARE NEEDED)")
end

-- Failing tests
cmd = rosie_cmd .. " test " .. TEST_HOME .. "/lightweight-test-fail.rpl 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0)
check(type(results[1])=="string")
check(code~=0, "Return code not zero")
if (#results <=0) or (code == 0) then
   print(cmd)
end

-- The last two output lines explain the test failures in our sample input file
local function split(s, sep)
   sep = lpeg.P(sep)
   local elem = lpeg.C((1 - sep)^0)
   local p = lpeg.Ct(elem * (sep * elem)^0)
   return lpeg.match(p, s)
end
if results[1]:find("Loading rosie from source") then
   table.remove(results, 1)
end
check(results[1]:find("lightweight-test-fail.rpl", 1, true))
check(results[2]:find("FAIL"))
check(results[3]:find("FAIL"))
check(results[4]:find("2 tests failed out of"))

---------------------------------------------------------------------------------------------------
test.heading("Config command")

cmd = rosie_cmd .. " config 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "config command failed")
check(code==0, "Return code is zero")
if (#results <=0) or (code ~= 0) then
   print(cmd)
end

txt = table.concat(results, '\n')
-- check for a few of the items displayed by the info command
check(txt:find("ROSIE_HOME"))      
check(txt:find("ROSIE_VERSION"))      
check(txt:find("ROSIE_COMMAND"))      

---------------------------------------------------------------------------------------------------
test.heading("Help command")

cmd = rosie_cmd .. " help 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command failed")
check(code==0, "Return code is not zero")
txt = table.concat(results, '\n')
check(txt:find("Usage:"))
check(txt:find("Options:"))
check(txt:find("Commands:"))
if (#results <=0) or (code ~= 0) then
   print(cmd)
end

---------------------------------------------------------------------------------------------------
test.heading("Error reporting")

cmd = rosie_cmd .. " -f test/nested-test.rpl grep foo test/resolv.conf 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command failed")
check(code ~= 0, "return code should not be zero")
if (#results <=0) or (code == 0) then
   print(cmd)
end
msg = table.concat(results)
check(msg:find('loader'))
check(msg:find('cannot open file'))
check(msg:find("in test/nested-test.rpl:2:1:", 1, true))

cmd = rosie_cmd .. " --libpath " .. TEST_HOME .. " -f test/nested-test2.rpl grep foo test/resolv.conf 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command failed")
check(code ~= 0, "return code should not be zero")
if (#results <=0) or (code == 0) then
   print(cmd)
end

msg = table.concat(results)
check(msg:find("Syntax error"))
check(msg:find("parser"))
check(msg:find("test/mod4.rpl:2:9:", 1, true))
check(msg:find("in test/nested-test2.rpl:6:3:", 1, true))

cmd = rosie_cmd .. " -f test/mod1.rpl grep foonet.any /etc/resolv.conf 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command failed")
check(code ~= 0, "return code should not be zero")
if (#results <=0) or (code == 0) then
   print(cmd)
end
msg = table.concat(results)
check(msg:find("error"))
check(msg:find("compiler"))
check(msg:find("unbound identifier"))
check(msg:find("foonet.any"))

cmd = rosie_cmd .. " -f test/mod4.rpl grep foonet.any /etc/resolv.conf 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command failed")
check(code ~= 0, "return code should not be zero")
if (#results <=0) or (code == 0) then
   print(cmd)
end
msg = table.concat(results)
check(msg:find("error"))
check(msg:find("parser"))
check(msg:find("in test/mod4.rpl:2:9"))
check(msg:find("package !@#"))

cmd = rosie_cmd .. " --libpath test -f test/nested-test3.rpl grep foo test/resolv.conf 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command failed")
check(code ~= 0, "return code should not be zero")
if (#results <=0) or (code == 0) then
   print(cmd)
end

msg = table.concat(results)
check(msg:find("error"))
check(msg:find("loader"))
check(msg:find("not a module"))
check(msg:find("in test/nested-test2.rpl", 1, true))
check(msg:find("in test/nested-test3.rpl:5:2", 1, true))

cmd = rosie_cmd .. " --rpl 'import net' list net.* 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command failed")
check(code == 0, "return code should be zero")
if (#results <=0) or (code ~= 0) then
   print(cmd)
end

for _, line in ipairs(results) do
   if line:sub(1,4)=="path" then
      check(line:find("green"))
      done1 = true
   elseif line:sub(1,4)=="port" then
      check(line:find("red"))
      done2 = true
   elseif line:sub(1,5)=="ipv6 " then		    -- distinguish from ipv6_mixed
      check(line:find("red;underline"))
      done3 = true
   end
end -- while
check(done1 and done2 and done3)

-- This command should fail gracefully
cmd = rosie_cmd .. " match -o json 'csv.XXXXXX' test/resolv.conf 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have failed with output")
check(code ~= 0, "return code should NOT be zero")
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)

cmd = rosie_cmd .. " grep 'net.any <\".com\"' test/resolv.conf 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
check(code == 0, "return should have been zero")
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)

cmd = rosie_cmd .. " grep '{net.any & num.int}' test/resolv.conf 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
check(code == 0, "return should have been zero")
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)

cmd = rosie_cmd .. " grep '(net.any & <\"search\")' test/resolv.conf 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
check(code ~= 0, "return should not have been zero")
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)
check(results_txt:find("can match the empty string"))

cmd = rosie_cmd .. " expand 'a b' 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
check(code == 0, "return should have been zero")
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)
check(results_txt:find("a ~ b"))

cmd = rosie_cmd .. " expand 'grammar foo=\"foo\" in foo' 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
check(code ~= 0, "return should NOT have been zero")
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)
check(results_txt:find("[parser]"))
check(results_txt:find("syntax error while reading expression"))

cmd = rosie_cmd .. " expand 'grammar foo=\"foo\" in bar=foo end' 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
check(code ~= 0, "return should NOT have been zero")
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)
check(results_txt:find("[parser]"))
check(results_txt:find("found statement where expression was expected"))

cmd = rosie_cmd .. " expand 'let foo=\"foo\" in bar=foo end' 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
check(code ~= 0, "return should NOT have been zero")
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)
check(results_txt:find("[parser]"))
check(results_txt:find("found statement where expression was expected"))

cmd = rosie_cmd .. " expand 'let foo=\"foo\" in foo' 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
check(code ~= 0, "return should NOT have been zero")
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)
check(results_txt:find("[parser]"))
check(results_txt:find("let expressions are not supported"))

cmd = rosie_cmd .. " expand 'grammar X foo=\"foo\" in foo' 2>&1"
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
check(code ~= 0, "return should NOT have been zero")
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)
check(results_txt:find("[parser]"))
check(results_txt:find("grammar expressions are not supported"))


---------------------------------------------------------------------------------------------------
test.heading("Colors")

function colortest(colors, expected_results)
   cmd = rosie_cmd ..
      " --colors '" .. colors .. 
      "' --rpl 'foo=\"nameserver\"' match 'foo net.any' test/resolv.conf 2>&1"
   local results, status, code = util.os_execute_capture(cmd, nil)
   check(#results>0, "command should have produced output", 1)
   check(code == 0, "return should have been zero", 1)
   results_txt = table.concat(results, '\n')
   check_lua_error(results_txt)
   check(results_txt:find(expected_results, 1, true),
         "results did not match expectations", 1)
   return results_txt
end

colortest("foo=cyan", [==[[36mnameserver[0m [m192.9.201.1[0m
[36mnameserver[0m [m192.9.201.2[0m
[36mnameserver[0m [mfde9:4789:96dd:03bd::1[0m]==])

colortest("foo=cyan:*=green", [==[
[36mnameserver[0m [32m192.9.201.1[0m
[36mnameserver[0m [32m192.9.201.2[0m
[36mnameserver[0m [32mfde9:4789:96dd:03bd::1[0m]==])

colortest("foo=cyan:*=ZZZ", "Warning: ignoring invalid color/attribute: ZZZ")

---------------------------------------------------------------------------------------------------
test.heading("Libpath")

--bin/rosie --libpath "test" --rpl 'import mod1' match 'mod1.S $' test/resolv.conf

function libpath_test(libpath, exit_status, expected_results)
   cmd = rosie_cmd ..
      " --libpath '" .. libpath .. 
      "' --rpl 'import mod1' match 'mod1.S $' test/resolv.conf 2>&1"
   local results, status, code = util.os_execute_capture(cmd, nil)
   check(#results>0, "command should have produced output", 1)
   check(code == exit_status, "exit status differs from expected", 1)
   results_txt = table.concat(results, '\n')
   check_lua_error(results_txt)
   for _, expectation in ipairs(expected_results) do
      check(results_txt:find(expectation, 1, true),
	    "results did not match expectations", 1)
   end
   return results_txt
end

libpath_test("x", 252, {"cannot open file x/mod1.rpl"})
libpath_test("x:y:::", 252, {"cannot open file x/mod1.rpl",
			     "cannot open file y/mod1.rpl",
			     "cannot open file /mod1.rpl"})
libpath_test("x:test", 0, {""})
libpath_test("test", 0, {""})

---------------------------------------------------------------------------------------------------
test.heading("Rcfile")

function rcfile_test(filename, exit_status, expected_results, set_no_rcfile)
   local norcfile = ""
   if set_no_rcfile then norcfile = " --norcfile "; end
   cmd = rosie_cmd .. norcfile .. " --rcfile '" .. filename .. "' config 2>&1"
   local results, status, code = util.os_execute_capture(cmd, nil)
   check(#results>0, "command should have produced output", 1)
   check(code == exit_status, "exit status differs from expected", 1)
   results_txt = table.concat(results, '\n')
   check_lua_error(results_txt)
   for _, expectation in ipairs(expected_results) do
      check(results_txt:find(expectation, 1, true),
	    "results did not match this expectation: " .. expectation, 1)
   end
   return results_txt
end

rcfile_test("this file does not exist", 0, {"Warning", "Could not open rcfile"})
rcfile_test("test/rcfile1", 0, {"Warning: [test/rcfile1]", "Failed to load another-file"})
rcfile_test("test/rcfile2", 0, {"Warning: [test/rcfile2]", "Syntax errors in rcfile"})
rcfile_test("test/rcfile3", 0, {"Warning: [test/rcfile3]", "Failed to load", "nofile_mod1.rpl"})
results = rcfile_test("test/rcfile4", 0, {'ROSIE_LIBPATH = "foo:bar:baz"',
					  'ROSIE_RCFILE = "test/rcfile4"',
					  'colors = "word.any=green'})
check(not results:find("Warning"))

results = rcfile_test("this file does not exist", 0, {'ROSIE_LIBPATH'}, true)
check(not results:find("Warning"))
check(not results:find("error"))

results = rcfile_test("test/rcfile1", 0, {}, true)
check(not results:find("Warning"))
check(not results:find("error"))


---------------------------------------------------------------------------------------------------
test.heading("Trace")


cmd = 'echo "1.2.3.4" | ' .. rosie_cmd .. ' trace net.ipv4 2>&1'
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
if not check(code == 0, "exit status differs from expected") then
   print("Command was:", cmd)
end
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)

cmd = 'echo "1.2.3.4" | ' .. rosie_cmd .. ' trace -o json net.ipv4 2>&1'
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
if not check(code == 0, "exit status differs from expected") then
   print("Command was:", cmd)
end
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)

cmd = 'echo "1.2.3.4" | ' .. rosie_cmd .. ' trace -o full net.ipv4 2>&1'
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
if not check(code == 0, "exit status differs from expected") then
   print("Command was:", cmd)
end
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)

cmd = 'echo "1.2.3.4" | ' .. rosie_cmd .. ' trace -o condensed net.ipv4 2>&1'
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
if not check(code == 0, "exit status differs from expected") then
   print("Command was:", cmd)
end
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)

cmd = 'echo "1.2.3.4" | ' .. rosie_cmd .. ' trace -o NOT_DEFINED net.ipv4 2>&1'
results, status, code = util.os_execute_capture(cmd, nil)
check(#results>0, "command should have produced output")
check(code ~= 0, "exit status differs from expected")
results_txt = table.concat(results, '\n')
check_lua_error(results_txt)


return test.finish()
