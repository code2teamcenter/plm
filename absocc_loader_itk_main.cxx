
/*==================================================================================================

                
   Date      Name                    Description of Change
29-Apr-2004  Michael Baker           initial implementation
07-May-2004  Michael Baker           remove use of Standard ANSI C++ and use old-style C++ (for HP & AIX)
07-May-2004  Michael Baker           fixed for compiling on HP
07-May-2004  Michael Baker           fix for AIX
13-May-2004  Michael Baker           implement suggested changes from code review
04-Jun-2004  Brendan Brolly          HP now compiled as AA and so uses same streams as everyone else
05-Jun-2004  Brendan Brolly          HP now built using -AA flag
27-May-2005  Jeff Martin             temporarily remove dataset creation, this needs to be revisited
01-Jun-2005  kaiser                  add more notes, seqno and qty
15-Jun-2005  kaiser                  merge
20-Jun-2005  kaiser                  removed DebugBreak()
13-Jul-2005  kaiser                  -xform option added
01-Sep-2005  Jo Stansfield           create ICs
$HISTORY$
==================================================================================================*/
#include <ctype.h>
#include <fclasses/tc_stdlib.h>
#include <fclasses/tc_stdio.h>
#include <fclasses/tc_string.h>
#include <errno.h>
#include <tcinit/tcinit.h>
#include <textsrv/textserver.h>
#include <tccore/aom_prop.h>
#include <ps/ps.h>
#include <bom/bom.h>
#include <bom/bom_attr.h>
#include <property/prop.h>
#include <ecm/ic.h>
#include <stdlib.h>
#include <time.h>

#include <iostream>
using namespace std;

#include <list>
#include <algorithm>
#include <vector>

#include <base_utils/status_t.hxx>
#include <fclasses/tcref.hxx>
#include <base_utils/String.hxx>
#include <sa/UserImpl.hxx>
#include <sa/GroupImpl.hxx>
#include <tc/loginuser.hxx>
#include <cxpom/pomtag.hxx>
#include <cxpom/cxpom.hxx>
#include <cxpom/tag.hxx>
#include <cxpom/tmobject.hxx>
#include <cxpom/tagmanager.hxx>
#include <fclasses/error_store.hxx>
#include <base_utils/ResultStatus.hxx>
#include <base_utils/IFail.hxx>
#include <tccore/ItemRevisionImpl.hxx>
#include <tccore/POM_objectImpl.hxx>
#include <bom/BOMWindowImpl.hxx>
#include <bom/BOMLineImpl.hxx>
#include <bom/GDELineImpl.hxx>
#include <cfm/RevisionRuleImpl.hxx>
#include <ae/DatasetImpl.hxx>
#include <ae/DatasetTypeImpl.hxx>
#include <form/FormImpl.hxx>
#include <tccore/ImanRelationImplPublic.hxx>
#include <metaframework/BusinessObjectRegistry.hxx>
#include <property/imanproperty.hxx>
#include <property/imanproperty_internal.hxx>
#include <ps/NoteTypeImpl.hxx>
#include <ps/PSBOMViewRevisionImpl.hxx>


/********************************************************************
* Declarations
********************************************************************/
void displayUsageMessage( bool verbose );
bool processArgOptions( int argc );
void traverseChildren( Teamcenter::BOMLineImpl& line );
void processForAbsOccData( Teamcenter::BOMLineImpl& line );
int  createICs();
int  setRandomIcContext();

/********************************************************************
* Global variables and constants
* (values intended to persist across all func calls!)
********************************************************************/
struct OptionContainer
{
    char *specified_item_id;
    char dsrelation_type_name[128];
    char frelation_type_name[128];
    bool abs_occ_xform;
    bool abs_occ_data_all_levels;
    int  abs_occ_data_levels;
    char dsTypeName[128];
    char formTypeName[128];
    int number_ic_ctxts;
} options;

int currDepth = 0;
std::list< Teamcenter::BOMLineImpl* > ancestorLines;
Teamcenter::BOMWindowImpl *window = 0;

int uniqueIdNumber = 0;
tag_t datasetTypeTag_t = NULLTAG;
tag_t datasetToolTag_t = NULLTAG;

Tag   dsRelationTypeTag;
Tag   fRelationTypeTag;

tag_t noteTypeTag[5];
std::vector<tag_t> ic;

#define NAME_IDX_OFFSET 6
#define MAX_UNIQUE_ID   999999999
char *uniqueId = (char*) "absocc                                                                "; // should be big enough to allow up to 999,999,999 ds!

/*----------------------------------------------------------------------------------------------------------*/

int ITK_user_main(int argc, char *[] )
{
    ////////////////////////////
    // Initialize basic services
    ////////////////////////////
    ITK_initialize_text_services(ITK_BATCH_TEXT_MODE);

    /////////////////////////////////
    // process command line arguments
    //  note: this sets the globals!!
    /////////////////////////////////
    if( !processArgOptions( argc ) )
    {
       exit (EXIT_FAILURE);
    }

    /////////////////////////////////////////////////
    // Login to database & be sure we have Privileges
    /////////////////////////////////////////////////
    if (ITK_auto_login() != ITK_ok)
    {
        cout << "unable to login" << endl;
        ITK_exit_module(TRUE);
        exit (EXIT_FAILURE);
    }

    if (loginUser::grantByPassPrivilege() != ITK_ok)
    {
       cout << "ERROR (" << ERROR_store_ask_last_ifail()
            << "): Unable to grant the loginuser the by-pass privilege." << endl;
       ITK_exit_module(TRUE);
       exit (EXIT_FAILURE);
    }

    ///////////////////////////////
    // Verify specified item exists
    ///////////////////////////////
    ImanItem topItem;
    if( (ImanItem::findItem( options.specified_item_id, topItem ) != ITK_ok)
        || topItem.tag() == NULLTAG )
    {
        cout    << "Unable to find specified top item: "
                << options.specified_item_id
                << endl;
       ITK_exit_module(TRUE);
       exit (EXIT_FAILURE);
    }

    //////////////////////////
    // Setup BOM Window to use
    //////////////////////////
    window = Teamcenter::BOMWindowImpl::createInstance();//dynamic_cast<BOMWindowImpl*>(BOMWindowGenImpl::newInstance());
    window->save();

        // set rule for window
    Teamcenter::RevisionRuleImpl *rule;
    Teamcenter::RevisionRuleImpl::find( "Latest Working", &rule );
    window->setRevisionRule( rule );
    window->save();

        // set top line
    Tag bvTag = NULLTAG;
    window->topLine( topItem, bvTag );
    Teamcenter::BOMLineImpl *topLine = window->topLine();
    window->save();

        // set edit mode
    window->setAbsOccEditMode( true );

    // create ICs
    createICs();

    // set the initial IC context
    if ( options.number_ic_ctxts > 0 )
    {
        window->setIncrChangeRev( ic[0] );
    }

    ////////////////////////////////////////////////
    // Setup for things needed throughout processing
    ////////////////////////////////////////////////
        // note type
    if( Teamcenter::NoteTypeImpl::find( "UG ALTREP", &noteTypeTag[0]) == NOT_OK )
    {
        cout << "ERROR(" << ERROR_store_ask_last_ifail()
             << "): can not get note type: UG ALTREP" << endl;
        exit( EXIT_FAILURE );
    }

    if( Teamcenter::NoteTypeImpl::find( "UG NAME", &noteTypeTag[1] ) == NOT_OK )
    {
        cout << "ERROR(" << ERROR_store_ask_last_ifail()
             << "): can not get note type: UG ALTREP" << endl;
        exit( EXIT_FAILURE );
    }

    if( Teamcenter::NoteTypeImpl::find( "UG GEOMETRY", &noteTypeTag[2] ) == NOT_OK )
    {
        cout << "ERROR(" << ERROR_store_ask_last_ifail()
             << "): can not get note type: UG ALTREP" << endl;
        exit( EXIT_FAILURE );
    }

    if( Teamcenter::NoteTypeImpl::find( "UG REF SET", &noteTypeTag[3] ) == NOT_OK )
    {
        cout << "ERROR(" << ERROR_store_ask_last_ifail()
             << "): can not get note type: UG ALTREP" << endl;
        exit( EXIT_FAILURE );
    }

    if( Teamcenter::NoteTypeImpl::find( "UG ENTITY HANDLE", &noteTypeTag[4] ) == NOT_OK )
    {
        cout << "ERROR(" << ERROR_store_ask_last_ifail()
             << "): can not get note type: UG ALTREP" << endl;
        exit( EXIT_FAILURE );
    }


        // dataset
    Teamcenter::DatasetTypeImpl::find( options.dsTypeName, &datasetTypeTag_t );
    Tag ds = datasetTypeTag_t;
    if( ds == NULLTAG )
    {
        cout << "ERROR(" << ERROR_store_ask_last_ifail()
             << "): can not get dataset type: "
             << options.dsTypeName << endl;
        exit( EXIT_FAILURE );
    }
    POMRef<Teamcenter::DatasetTypeImpl, true> dstype(ds);
    if( !dstype.areYouA<Teamcenter::DatasetTypeImpl>() )
    {
        cout << "ERROR(" << ERROR_store_ask_last_ifail()
             << "): can not access dataset type:"
             << options.dsTypeName << endl;
        exit( EXIT_FAILURE );
    }
    datasetToolTag_t = dstype->defaultToolTag();
    if( datasetToolTag_t == NULLTAG )
    {
        cout << "ERROR(" << ERROR_store_ask_last_ifail()
             << "): can not access default tool for dataset type:"
             << options.dsTypeName << endl;
        exit( EXIT_FAILURE );
    }

        // relation types
    ImanRelationImplPublic::findRelationType( options.dsrelation_type_name, dsRelationTypeTag );
    if( dsRelationTypeTag == NULLTAG )
    {
        cout << "ERROR("<< ERROR_store_ask_last_ifail() <<"): can not get relation type "
             << options.dsrelation_type_name << endl;
        exit( EXIT_FAILURE );
    }

    ImanRelationImplPublic::findRelationType( options.frelation_type_name, fRelationTypeTag );
    if( fRelationTypeTag == NULLTAG )
    {
        cout << "ERROR(" << ERROR_store_ask_last_ifail() << "): can not get relation type "
             << options.frelation_type_name << endl;
        exit( EXIT_FAILURE );
    }

    //////////////////////////////////////////////
    // start recursing down the product structure!
    //////////////////////////////////////////////
    traverseChildren( *topLine );

    //////////
    // Cleanup
    //////////
    return ITK_exit_module (TRUE);
}
/* End of main program */

/*----------------------------------------------------------------------------------------------------------*/
bool processArgOptions( int argc )
{
        // Help //
    if( ITK_ask_cli_argument("-h") != NULL )
    {
        displayUsageMessage( true );
        return( false );
    }

        // Top Line //
    options.specified_item_id = ITK_ask_cli_argument("-t=");

        // Levels to be processed (2 parts)
    if( ITK_ask_cli_argument("-a") == NULL )
    {
        options.abs_occ_data_all_levels = false;
    }
    else
    {
        options.abs_occ_data_all_levels = true;
    }


    if( ITK_ask_cli_argument("-xform") == NULL )
    {
        options.abs_occ_xform = false;
    }
    else
    {
        options.abs_occ_xform = true;
    }


    options.abs_occ_data_levels = 0;
    char *tmpLvl = ITK_ask_cli_argument("-d=");
    if( tmpLvl != NULL )
    {
        options.abs_occ_data_levels = atoi( tmpLvl );
        if( options.abs_occ_data_levels < 2 )
            options.abs_occ_data_levels = 2;
    }

    options.number_ic_ctxts = 0;
    char *tmpIc = ITK_ask_cli_argument( "-n_ic=" );
    if( tmpIc != NULL )
    {
        options.number_ic_ctxts = atoi( tmpIc );
        if ( options.number_ic_ctxts < 0 )
             options.number_ic_ctxts = 0;
    }

    char *tmpRel;
    tmpRel = ITK_ask_cli_argument("-rd=");
    if( tmpRel != 0 )
    {
        strcpy( options.dsrelation_type_name, tmpRel );
    }
    else
    {
        strcpy( options.dsrelation_type_name, "IMAN_reference" );
    }

    tmpRel = ITK_ask_cli_argument("-rf=");
    if( tmpRel != 0 )
    {
        strcpy( options.frelation_type_name, tmpRel );
    }
    else
    {
        strcpy( options.frelation_type_name, "IMAN_reference" );
    }

    char *tmpName;
    tmpName = ITK_ask_cli_argument( "-dtype=" );
    if( tmpName == 0 )
    {
        strcpy( options.dsTypeName, "Text" );
    }
    else
    {
        strcpy( options.dsTypeName, tmpName );
    }

    tmpName = ITK_ask_cli_argument( "-ftype=" );
    if( tmpName == 0 )
    {
        strcpy( options.formTypeName, "COForm" );
    }
    else
    {
        strcpy( options.formTypeName, tmpName );
    }

    ////////////////////////////////////
    // Sanity check specified parameters
    // abort if insane!
    ////////////////////////////////////
    if( (!options.abs_occ_data_all_levels && options.abs_occ_data_levels < 2 )
        || (options.specified_item_id == NULL )
        || argc < 4 )
    {
        displayUsageMessage( false );
        return( false );
    }

    return( true );
}

/*----------------------------------------------------------------------------------------------------------*/
void displayUsageMessage( bool verbose )
{
    if( verbose )
    {
        cout << "This utility is for adding absolute occurrence data to an existing structure.  ";
        cout << "Only the latest version will have absolute data added to it." << endl;
        cout << "If both '-a' and '-d' are specified, data will be generated for all levels.";
        cout << "If a number less than 2 is given for the '-d' argument, 2 will be assumed." << endl;
        cout << "This utility is for internal use only." << endl;
    }

    cout << endl;
    cout << "-u=<user_id>    specfies the user id for logging in" << endl;
    cout << "-p=<pass_wd>    specfies the user password for logging in" << endl;
    cout << "-pf=<pass_file> specfies the user password in a file" << endl;
    cout << "-g=<group>      specfies the group the user belongs to for logging in" << endl;
    cout << endl;
    cout << "-t=<item_id>    specifies the unique id of the top level item for the structure" << endl;
    cout << "-a              absolute occurrence data is to be generated for all children" << endl;
    cout << "                   (default = on)" << endl;
    cout << "-d=<# levels>   specifies the max depth to generate absolute occurrence data for" << endl;
    cout << "-rd=<relname>   name of the relation type for attaching Datasets" << endl;
    cout << "                   (default = IMAN_reference)" << endl;
    cout << "-rf=<relname>   name of the relation type for attaching Forms" << endl;
    cout << "                   (default = IMAN_specification)" << endl;
    cout << "-dtype=<name>   name of the datasaet type to use" << endl;
    cout << "                   (default = Text)" << endl;
    cout << "-ftype=<name>   name of the form type to use" << endl;
    cout << "                   (default = COForm)" << endl;
    cout << "-xform          create one xform override per line in toplevel context" << endl;
    cout << "-n_ic           number of IC contexts used (default=0)" << endl;
    return;
}

/*----------------------------------------------------------------------------------------------------------*/
// Globals used only by following function
// NOTE: these are defined as global for performance reasons so
// space is not continually allocated on the stack!
// DO NOT USE OUTSIDE OF "traverseChildren"

void traverseChildren( Teamcenter::BOMLineImpl& line )
{
    /////////////////////////////////
    // abort if we have gone too deep
    /////////////////////////////////
    if(     !options.abs_occ_data_all_levels
        &&  currDepth == options.abs_occ_data_levels )
        return;
    else
        currDepth++;    // mark depth as deeper

    // Set the IC Context
    if ( options.number_ic_ctxts > 1 )
    {
        setRandomIcContext();
    }

    /////////////////////////////////////////////////////
    // Process this line to add absolute occurrence data!
    /////////////////////////////////////////////////////
    processForAbsOccData( line );

    //////////////////////////////
    // process all of the children
    //////////////////////////////
    ancestorLines.push_back( &line );

        // Note: these variables are needed on the stack!
    int childCount;
    Teamcenter::BOMLineImpl **childLines;

    line.listChildLines( &childCount, &childLines );
    for( int currLine=0; currLine < childCount; currLine++ )
    {
        if( !dynamic_cast<Teamcenter::GDELineImpl *>(childLines[currLine]) )
            traverseChildren( *childLines[currLine] );
    }

    ancestorLines.pop_back();

    currDepth--;    // update depth to not be so deep
}

/*----------------------------------------------------------------------------------------------------------*/
// Globals used only by following function
// NOTE: these are defined as global for performance reasons so
// space is not continually allocated on the stack!
// DO NOT USE OUTSIDE OF "processForAbsOccData"
std::list<Teamcenter::BOMLineImpl*>::iterator currAncestor;
Teamcenter::DatasetImpl         *pDataset;
Teamcenter::FormImpl            *pForm;
ImanProperty    *pProperty;
tag_t           dummy_t = NULLTAG;
logical         isNull  = false;
char            uniqueIdNumberString[10];

void processForAbsOccData( Teamcenter::BOMLineImpl& line )
{
    // if only immediate parent as ancestor, skip
    // cann't have absocc data for immediate parent!
    if( ancestorLines.size() < 1 )
        return;

    /////////////////////////////
    // iterate over all ancestors
    /////////////////////////////

        // start with highest level ancestor and work down to parent
        // skip immediate parent (last one in list!)
    for(    currAncestor = ancestorLines.begin();
                (*currAncestor != ancestorLines.back())
            &&  (currAncestor != ancestorLines.end());
            currAncestor++ )
    {
            // set context
        window->setCtxtLine( (*currAncestor)->tag() );

            // create dataset & form (one for each line!)
            // (note: don't worry about hitting the same IR 2x
            //        since multiple occ in struct are diff absocc!)
        if( !options.abs_occ_xform)
        {
           // sprintf( uniqueIdNumberString, "%d", uniqueIdNumber );
           //sprintf( (uniqueId + NAME_IDX_OFFSET), "%d", uniqueIdNu

            for (int kk=0; kk<5; kk++)
                if( line.setOccurrenceNote( noteTypeTag[kk], uniqueId ) == NOT_OK )
            {
                cout << "ERROR(:" << ERROR_store_ask_last_ifail()
                     << "): can not set occurrence note" << endl;
                exit( EXIT_FAILURE );
            }

            // set quantity
            line.EIMObject::askProperty( "bl_quantity", &pProperty );
            pProperty->setValue( "123", isNull );

            // sequence number
            line.EIMObject::askProperty( "bl_sequence_no", &pProperty );
            pProperty->setValue( "10", isNull );

            // occtype
            line.EIMObject::askProperty( "bl_occ_type", &pProperty );
            pProperty->setValue( "METarget", isNull );

        }
        else if (currAncestor == ancestorLines.begin())
        {
            // only attach xform in highest context!
            char *ParentAbsMatrix;
            tag_t ltag = line.tag();
            int transform_attribute;
            if (BOM_line_look_up_attribute( bomAttr_OccTransformMatrix, &transform_attribute) != ITK_ok ||
                BOM_line_ask_attribute_string( ltag, transform_attribute, &ParentAbsMatrix) != ITK_ok ||
                BOM_line_look_up_attribute( bomAttr_AbsTransformMatrix, &transform_attribute) != ITK_ok ||
                BOM_line_set_attribute_string( ltag, transform_attribute, ParentAbsMatrix) != ITK_ok)
                    continue; // just ignore errors
        }
    }

    line.save();
    window->save(); // not needed???
}

int createICs()
{
    try
    {
        // Create the incremental change items
        String ICItemBaseName = "Absocc_loader_ic";
        ResultStatus stat;
        tag_t ic_item;
        tag_t ic_rev;
        char id[256];

        for ( int i = 0; i < options.number_ic_ctxts; ++i )
        {
            char num[17];
            sprintf( num, "%d", i );
            String name = ICItemBaseName + num;
            stat = ECM_create_ec_item( "", "", name.gets(), "", &ic_item, &ic_rev );

            // report
            stat = ITEM_ask_id( ic_item, id );
            printf( "Created Incremental Change Item, ID:, %s,\tName: %s,\tRevision:, A\n", id, name.gets() );

            ic.push_back( ic_rev );
        }
    }
    catch ( IFail& i )
    {
        return i;
    }

    // seed random number for later
    srand( (unsigned int)time( NULL ) );

    return ITK_ok;
}

int setRandomIcContext()
{
    try
    {
        if ( options.number_ic_ctxts > 1 )
        {
            int n = rand()%options.number_ic_ctxts;
            tag_t ic_rev = ic[ n ];
            window->setIncrChangeRev( ic_rev );
            printf( "Using IC %d\n", n );
        }
    }
    catch ( IFail& i )
    {
        return i;
    }
    return ITK_ok;
}
