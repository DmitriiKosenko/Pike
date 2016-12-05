inherit "../test.pike";

constant BASE = __DIR__;

int run()
{
  werror("Run in test: %s\n", basename(BASE));

  compiler->include_path = BASE;

  mixed err = catch {
    compiler->compile_file(combine_path(BASE, "input.scss"),
                           combine_path(BASE, "output.css"));
  };

  handle_err(err);

  string cont = Stdio.read_file(combine_path(BASE, "input.scss"));

  err = catch {
    compiler->compile_string(cont);
  };

  handle_err(err);
}
