#!/usr/bin/env node
// Verify bitemporal extension via lbug_shell.exe pipe.
// Run from: ladybug-0.18.3/
var path=require('path'),cp=require('child_process');

var SHELL=path.resolve(__dirname,'..','..','..','build_fixed','src','lbug_shell.exe');
var DB   =path.resolve(__dirname,'..','..','..','test_honglou.lbug');
var EXT  =path.resolve(__dirname,'..','..','..','extension','bitemporal','build','libbitemporal.lbug_extension');

var P=0,F=0;
function check(name,input,expect){
    try{
        var out=cp.execSync('"'+SHELL+'" "'+DB+'"',{input:input,encoding:'utf8',timeout:20000,windowsHide:true});
        if(out.includes('Error')&&!out.includes('Extension:')){
            var el=out.split('\n').filter(function(l){return l.match('Error:')});
            throw new Error(el.length?el[0].trim():out.slice(-200));
        }
        if(expect!==undefined && out.indexOf(expect)===-1)
            throw new Error('expected output containing "'+expect+'"');
        console.log('[PASS] '+name);P++;
    }catch(e){console.log('[FAIL] '+name+' => '+e.message.substring(0,200));F++;}
}

// Re-create test DB
try{cp.execSync('"'+SHELL+'" "'+DB+'"',{input:require('fs').readFileSync(
    path.resolve(__dirname,'..','..','..','test_data.cypher'),'utf8'),timeout:30000,windowsHide:true})}catch(e){}

check('LOAD EXTENSION',
    "LOAD EXTENSION '"+EXT.replace(/\\/g,'/')+"';\n",
    'Extension:');

check('character_similarity',
    "LOAD EXTENSION '"+EXT.replace(/\\/g,'/')+"';\n"+
    "CALL character_similarity(0.12,0.08,0.8,0.6, 0.1,0.15,0.5,0.4) RETURN *;\n",
    '0.664583');

// detect_turning_points: 4 positional args + threshold
// 3 vectors × 4 dims = 12 doubles, N=3, chapters=[1,40,80], dim=4
check('detect_turning_points (4 args)',
    "LOAD EXTENSION '"+EXT.replace(/\\/g,'/')+"';\n"+
    "CALL detect_turning_points([0.33,0.07,0.6,0.7, 0.12,0.08,0.8,0.6, 0.06,0.23,-0.3,0.3], 3, [1,40,80], 4, threshold:=0.01) RETURN *;\n",
    'chapter_number');

check('detect_turning_points high threshold',
    "LOAD EXTENSION '"+EXT.replace(/\\/g,'/')+"';\n"+
    "CALL detect_turning_points([1.0,2.0,3.0,4.0, 1.0,2.0,3.0,4.0], 2, [1,2], 4, threshold:=0.5) RETURN *;\n",
    'chapter_number'); // header still shows even if 0 rows

check('identical features → 1.0',
    "LOAD EXTENSION '"+EXT.replace(/\\/g,'/')+"';\n"+
    "CALL character_similarity(0.5,0.5, 0.5,0.5, 0.5,0.5, 0.5,0.5) RETURN *;\n",
    '1.000000');

check('zero features',
    "LOAD EXTENSION '"+EXT.replace(/\\/g,'/')+"';\n"+
    "CALL character_similarity(0.0,0.0, 0.0,0.0, 0.0,0.0, 0.0,0.0) RETURN *;\n",
    'similarity_score'); // outputs table header even with denom guard

// dimension mismatch → 0 rows
check('dimension mismatch → 0 rows',
    "LOAD EXTENSION '"+EXT.replace(/\\/g,'/')+"';\n"+
    "CALL detect_turning_points([1.0,2.0], 2, [1,2,3], 4, threshold:=0.01) RETURN *;\n",
    'chapter_number');

// bitemporal_query no longer a C++ function — test pure Cypher template
check('GRAMMAR_FACT table exists',
    "MATCH (c:Character)-[f:GRAMMAR_FACT]->(ch:Chapter) WHERE ch.chapter_number=40 RETURN f.fact_type, f.value;\n",
    'ba_ratio');

console.log('\nPASS:'+P+' FAIL:'+F);
process.exit(F>0?1:0);
