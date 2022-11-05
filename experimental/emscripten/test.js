var fs = require('fs');
var tinydng = require('./tinydng.js')

tinydng.onRuntimeInitialized = async function() {
    var data = new Uint8Array(fs.readFileSync("../../colorchart.dng"))
    //console.log(data.length)

    var instance = new tinydng.DNGLoader(data);
    console.log("result " + instance.ok());
    console.log("largest_idx " + instance.largest_idx());
    console.log("width " + instance.width());
    console.log("height " + instance.height());
    console.log("channels " + instance.channels());
    console.log("bits " + instance.bits());

    var image = instance.getImageBytes();
}

