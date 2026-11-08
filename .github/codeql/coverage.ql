/** Lists parsed function bodies in compiled project sources, not just archived files. */
import cpp

from File source, Function fn
where
  exists(Compilation compilation | compilation.getAFileCompiled() = source) and
  fn.getBlock().getFile() = source and
  (
    source.getRelativePath().matches("firmware/main/%.cpp") or
    source.getRelativePath().matches("components/wiknow/src/%.cpp") or
    source.getRelativePath() = "tests/core_tests.cpp"
  )
select source.getRelativePath() as file, fn.getName() as function_name
