module run.arguments_count

capability Arguments {
    function count() -> Int64 effects { process.arguments.count<Self> }
    function get(index: Int64) -> Option<Text> effects { process.arguments.get<Self> }
}

@entry
public function launch(arguments: capability Arguments) -> Int64
effects { process.arguments.count<arguments> } {
    return arguments.count()
}
