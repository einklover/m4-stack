-- Shared provider template; implementation is kept in one package source.
if type(sys) == "table" and type(sys.load) == "function" then
  sys.load("ui_template_shared.lua")
else
  dofile("test/fixtures/m4x/fanqie_src/ui_template_shared.lua")
end
