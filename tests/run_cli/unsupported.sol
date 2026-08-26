module run.unsupported

capability Console {
    function write(value: Text) -> () effects { console.write<Self> }
}

capability FileSystem {
    function read(path: Text) -> Text effects { filesystem.read<Self> }
}

@entry
public function launch(console: capability Console, filesystem: capability FileSystem) -> ()
effects { console.write<console> filesystem.read<filesystem> } {
    console.write("must not run")
    let content = filesystem.read("file")
}
