module run.console

capability Console {
    function write(value: Text) -> () effects { console.write<Self> }
}

@entry
public function launch(console: capability Console) -> ()
effects { console.write<console> } {
    console.write("hello")
    console.write(" world\n")
}
