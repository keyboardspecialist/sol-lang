module run.arguments

capability Arguments {
    function count() -> Int64 effects { process.arguments.count<Self> }
    function get(index: Int64) -> Option<Text> effects { process.arguments.get<Self> }
}

@entry
public function launch(arguments: capability Arguments) -> Int64
effects { process.arguments.count<arguments> process.arguments.get<arguments> } {
    if arguments.get(-1) == none()
        && arguments.get(99) == none()
        && arguments.get(0) == some("first") {
        return arguments.count()
    } else {
        return 100
    }
}
