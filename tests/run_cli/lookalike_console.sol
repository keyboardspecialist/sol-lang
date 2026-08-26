module run.lookalike_console

capability Console {
    function write(value: Text) -> Int64 effects { console.write<Self> }
}

@entry
public function launch(console: capability Console) -> ()
effects { console.write<console> } {
    let written = console.write("must not run")
}
