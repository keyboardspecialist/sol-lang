module run.configuration

capability Configuration {
    function read(key: Text) -> Option<Text> effects { configuration.read<Self> }
}

@entry
public function launch(configuration: capability Configuration) -> Int64
effects { configuration.read<configuration> } {
    if configuration.read("mode") == some("test") {
        return 7
    } else {
        return 1
    }
}
