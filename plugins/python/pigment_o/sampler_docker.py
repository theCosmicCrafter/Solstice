# Pigment.O is a Krita plugin and it is a Color Picker and Color Mixer and Color Sampler.
# Copyright ( C ) 2020  Ricardo Jeremias.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# ( at your option ) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.


#region Import Modules

# Python
import webbrowser
# Krita
from krita import *
# PyQt5
from PyQt5 import QtWidgets, QtCore, QtGui, uic
# Engine
from .engine_constants import *
from .engine_calculations import *
# Picker
from .sampler_modulo import *

#endregion


class Sampler_Docker( DockWidget ):
    """
    Color Sampler
    """

    #region Initialize

    def __init__( self ):
        super( Sampler_Docker, self ).__init__()

        # Construct
        self.Interface()
        self.Module()
        self.Style()
        self.Variable()
        self.Connection()

    def Interface( self ):
        # Window
        self.setWindowTitle( DOCKER_SAMPLER )
        # Path Name
        self.directory_plugin = str( os.path.dirname( os.path.realpath( __file__ ) ) )
        # Widget Docker
        self.layout = uic.loadUi( os.path.join( self.directory_plugin, "sampler_docker.ui" ), QWidget( self ) )
        self.setWidget( self.layout )
        # Settings
        self.dialog = uic.loadUi( os.path.join( self.directory_plugin, "sampler_settings.ui" ), QDialog( self ) )
        self.dialog.setWindowTitle( f"{ DOCKER_SAMPLER } : Settings" )
        self.dialog.accept() # Hides the Dialog
        # Settings
        self.dlut = uic.loadUi( os.path.join( self.directory_plugin, "sampler_lut.ui" ), QDialog( self ) )
        self.dlut.setWindowTitle( f"{ DOCKER_SAMPLER } : LUT" )
        self.dlut.accept() # Hides the Dialog
    def Module( self ):
        #region Notifier

        self.notifier = Krita.instance().notifier()
        self.notifier.applicationClosing.connect( self.Application_Closing )
        self.notifier.configurationChanged.connect( self.Configuration_Changed )
        self.notifier.imageClosed.connect( self.Image_Closed )
        self.notifier.imageCreated.connect( self.Image_Created )
        self.notifier.imageSaved.connect( self.Image_Saved )
        self.notifier.viewClosed.connect( self.View_Closed )
        self.notifier.viewCreated.connect( self.View_Created )
        self.notifier.windowCreated.connect( self.Window_Created )
        self.notifier.windowIsBeingCreated.connect( self.Window_IsBeingCreated )

        #endregion
        #region Calculations

        # Convert
        self.convert = Convert()
        # Analyse
        self.analyse = Analyse()

        #endregion
        #region Modules

        # Display
        self.display_map = Display_Map( self.layout.display_map )
        self.display_map.SIGNAL_INSERT.connect( self.Mask_Insert )
        self.display_map.SIGNAL_CLEAN.connect( self.Mask_Clean )

        # Controller
        self.channel_select = Channel_Select( self.layout.channel_select )
        self.channel_select.SIGNAL_INDEX.connect( self.Channel_Index )

        # Range 1
        self.range_0 = Channel_Slider( self.layout.range_0 )
        self.range_0.SIGNAL_PA.connect( self.Range_0_PA )
        self.range_0.SIGNAL_PB.connect( self.Range_0_PB )
        self.range_0.SIGNAL_PC.connect( self.Range_0_PC )
        self.range_0.SIGNAL_PD.connect( self.Range_0_PD )
        # Range 2
        self.range_1 = Channel_Slider( self.layout.range_1 )
        self.range_1.SIGNAL_PA.connect( self.Range_1_PA )
        self.range_1.SIGNAL_PB.connect( self.Range_1_PB )
        self.range_1.SIGNAL_PC.connect( self.Range_1_PC )
        self.range_1.SIGNAL_PD.connect( self.Range_1_PD )
        # Range 3
        self.range_2 = Channel_Slider( self.layout.range_2 )
        self.range_2.SIGNAL_PA.connect( self.Range_2_PA )
        self.range_2.SIGNAL_PB.connect( self.Range_2_PB )
        self.range_2.SIGNAL_PC.connect( self.Range_2_PC )
        self.range_2.SIGNAL_PD.connect( self.Range_2_PD )
        # Range 4
        self.range_3 = Channel_Slider( self.layout.range_3 )
        self.range_3.SIGNAL_PA.connect( self.Range_3_PA )
        self.range_3.SIGNAL_PB.connect( self.Range_3_PB )
        self.range_3.SIGNAL_PC.connect( self.Range_3_PC )
        self.range_3.SIGNAL_PD.connect( self.Range_3_PD )

        #endregion
    def Style( self ):
        self.Style_Icon()
        self.Style_Tooltip()
        self.Style_Theme()
    def Variable( self ):
        #region Pigmento

        # Color Picker Module
        self.pigmento_picker = None
        self.pigmento_picker_pyid = "pykrita_pigment_o_picker_docker"

        #endregion
        #region Layout

        # Krita
        self.kcanvas = None
        self.kview = None

        # Footer
        self.color_space = Kritarc_Read( DOCKER_SAMPLER, "color_space", "SRGB", str )
        self.split_method = Kritarc_Read( DOCKER_SAMPLER, "split_method", "CHANNEL", str ) # "CHANNEL" "RANGE"

        # Neutral
        self.qpixmap_logo = QPixmap( os.path.join( self.directory_plugin, "ICON\\SAMPLER.png" ) )
        self.time = None

        # Channel
        self.channel_data = list()
        self.channel_index = 0

        # Range
        self.range_data = list()
        # Range 1
        self.range_0_pa = 0.3
        self.range_0_pb = 0.4
        self.range_0_pc = 0.6
        self.range_0_pd = 0.7
        # Range 2
        self.range_1_pa = 0.3
        self.range_1_pb = 0.4
        self.range_1_pc = 0.6
        self.range_1_pd = 0.7
        # Range 3
        self.range_2_pa = 0.3
        self.range_2_pb = 0.4
        self.range_2_pc = 0.6
        self.range_2_pd = 0.7
        # Range 4
        self.range_3_pa = 0.3
        self.range_3_pb = 0.4
        self.range_3_pc = 0.6
        self.range_3_pd = 0.7

        #endregion
        #region Dialog

        # Color Space
        self.cs_luminosity = Kritarc_Read( DOCKER_SAMPLER, "cs_luminosity", "Rec.709", str )
        self.cs_matrix = Kritarc_Read( DOCKER_SAMPLER, "cs_matrix", "sRGB (D50)", str )
        # Maps
        self.tic_display = Kritarc_Read( DOCKER_SAMPLER, "tic_display", False, eval )
        self.tic_invert = Kritarc_Read( DOCKER_SAMPLER, "tic_invert", False, eval )
        self.tic_value = Kritarc_Read( DOCKER_SAMPLER, "tic_value", 300, eval )
        self.cmyk_invert = Kritarc_Read( DOCKER_SAMPLER, "cmyk_invert", False, eval )

        #endregion
        #region LUT

        self.lut_list_url = list()
        self.lut_file_url = Kritarc_Read( DOCKER_SAMPLER, "lut_file_url", str(), str )
        self.lut_deposit_url = Kritarc_Read( DOCKER_SAMPLER, "lut_deposit_url", str(), str )

        #endregion
    def Connection( self ):
        #region Layout
        
        # Workflow
        self.layout.color_space.currentTextChanged.connect( self.Color_Space );   self.layout.color_space.setCurrentText( self.color_space );   self.Color_Space( self.color_space )
        self.layout.split_method.currentTextChanged.connect( self.Split_Method ); self.layout.split_method.setCurrentText( self.split_method ); self.Split_Method( self.split_method )
        self.layout.mask_generate.clicked.connect( self.Mask_Generate )
        self.layout.mask_insert.clicked.connect( self.Mask_Insert )
        # Settings
        self.layout.settings.clicked.connect( self.Menu_Settings )

        #endregion
        #region Dialog

        # Color Space
        self.dialog.cs_luminosity.currentTextChanged.connect( self.CS_Luminosity ); self.dialog.cs_luminosity.setCurrentText( self.cs_luminosity ); self.CS_Luminosity( self.cs_luminosity )
        self.dialog.cs_matrix.currentTextChanged.connect( self.CS_Matrix ); self.dialog.cs_matrix.setCurrentText( self.cs_matrix ); self.CS_Matrix( self.cs_matrix )
        # Map
        self.dialog.tic_display.toggled.connect( self.TIC_Display );  self.dialog.tic_display.setChecked( self.tic_display ); self.TIC_Display( self.tic_display )
        self.dialog.tic_invert.toggled.connect( self.TIC_Invert );    self.dialog.tic_invert.setChecked( self.tic_invert );   self.TIC_Invert( self.tic_invert )
        self.dialog.tic_value.valueChanged.connect( self.TIC_Value ); self.dialog.tic_value.setValue( self.tic_value );       self.TIC_Value( self.tic_value )
        self.dialog.cmyk_invert.toggled.connect( self.CMYK_Invert );  self.dialog.cmyk_invert.setChecked( self.cmyk_invert ); self.CMYK_Invert( self.cmyk_invert )
        # Notices
        self.dialog.manual.clicked.connect( self.Menu_Manual )
        self.dialog.license.clicked.connect( self.Menu_License )

        #endregion
        #region Color

        # LUt
        self.dlut.lut_file_url.textChanged.connect( self.LUT_File_URL );  self.dlut.lut_file_url.setText( self.lut_file_url ); self.LUT_File_URL( self.lut_file_url )
        self.dlut.lut_file_dialog.clicked.connect( self.LUT_File_Dialog )
        # Destination
        self.dlut.lut_deposit_url.textChanged.connect( self.LUT_Deposit_URL );  self.dlut.lut_deposit_url.setText( self.lut_deposit_url ); self.LUT_Deposit_URL( self.lut_deposit_url )
        self.dlut.lut_deposit_dialog.clicked.connect( self.LUT_Deposit_Dialog )
        # Decision
        self.dlut.lut_accept.clicked.connect( self.LUT_Accept )
        self.dlut.lut_abort.clicked.connect( self.LUT_Abort )

        #endregion
        #region Event Filters

        self.layout.settings.installEventFilter( self ) # Photoshoot

        #endregion

    #endregion
    #region Managment

    # Pigmento
    def Import_Picker( self ):
        # Variables
        self.pigmento_picker = None
        # Search Dockers
        docker_list = Krita.instance().dockers()
        for docker in docker_list:
            try: # Module Picker
                if docker.objectName() == self.pigmento_picker_pyid:
                    self.pigmento_picker = docker
                    break
            except:
                pass

    # Widgets
    def ProgressBar_Value( self, value ):
        self.layout.progress_bar.setValue( value )
    def ProgressBar_Maximum( self, maximum ):
        self.layout.progress_bar.setMaximum( maximum )
    def Widgets_Enabled( self, boolean ):
        self.layout.footer.setEnabled( boolean )
        self.dialog.setEnabled( boolean )
    def Widget_Insert( self, boolean ):
        self.layout.mask_insert.setEnabled( boolean )
        self.layout.mask_insert.setFlat( not boolean )

    # File Dialog
    def Open_Directory( self, string, directory ):
        file_dialog = QFileDialog( QWidget( self ) )
        file_dialog.setFileMode( QFileDialog.DirectoryOnly )
        folder_url = file_dialog.getExistingDirectory( self, string, directory )
        folder_url = os.path.normpath( folder_url )
        return folder_url
    def Open_File( self, string, directory, extension ):
        file_dialog = QFileDialog( QWidget( self ) )
        file_dialog.setFileMode( QFileDialog.DirectoryOnly )
        file_url = file_dialog.getOpenFileName( self, string, directory, f"File( *.{ extension } )" )[0]
        file_url = os.path.normpath( file_url )
        return file_url

    # Resize Event
    def Size_Print( self ):
        width = self.width()
        height = self.height()
        Message_Log( "SIZE", f"{ width } x { height }" )
    def Size_Standard( self ):
        if self.isFloating() == True:
            self.resize( QSize( 500, 500 ) )
            self.Size_Update()
    def Size_Update( self ):
        # Modules
        self.display_map.Set_Size( self.layout.display_map.width(), self.layout.display_map.height() )
        # Channel
        self.channel_select.Set_Size( self.layout.channel_select.width(), self.layout.channel_select.height() )
        # Range
        self.range_0.Set_Size( self.layout.range_0.width(), self.layout.range_0.height() )
        self.range_1.Set_Size( self.layout.range_1.width(), self.layout.range_1.height() )
        self.range_2.Set_Size( self.layout.range_2.width(), self.layout.range_2.height() )
        self.range_3.Set_Size( self.layout.range_3.width(), self.layout.range_3.height() )
        # Update
        self.update()

    # Style
    def Style_Icon( self ):
        # Variables
        ki = Krita.instance()
        # Icons
        qicon_run = ki.icon( "animation_play" )
        qicon_settings = ki.icon( "settings-button" )
        self.qicon_insert = "krita_tool_enclose_and_fill"
        self.qicon_folder = ki.icon( "document-open" )
        # Layout
        self.layout.mask_generate.setIcon( qicon_run )
        self.layout.mask_insert.setIcon( ki.icon( self.qicon_insert ) )
        self.layout.settings.setIcon( qicon_settings )
        # Lut
        self.dlut.lut_file_dialog.setIcon( self.qicon_folder )
        self.dlut.lut_deposit_dialog.setIcon( self.qicon_folder )
    def Style_Tooltip( self ):
        self.layout.color_space.setToolTip( "Color Space" )
        self.layout.split_method.setToolTip( "Split Method" )
        self.layout.mask_generate.setToolTip( "Mask Generate" )
        self.layout.mask_insert.setToolTip( "Mask Insert" )
        self.layout.settings.setToolTip( "Settings" )
    def Style_Theme( self ):
        """
        Theme Breeze Dark
        w_alternate     = #31363b
        w_base          = #232629
        w_button        = #31363b
        w_dark          = #1d2023
        w_light         = #464d54
        w_mid           = #2b3034
        w_midlight      = #3c4248
        w_shadow        = #151719
        w_tool_tip      = #31363b
        w_window        = #31363b
        t_bright        = #ffffff
        t_button        = #eff0f1
        t_highlighted   = #eff0f1
        t_placeholder   = #eff0f1
        t_text          = #eff0f1
        t_tool_tip      = #eff0f1
        t_window        = #eff0f1
        c_highlight     = #3daee9
        c_link          = #2980b9
        c_visited       = #7f8c8d

        Theme Breeze Ligh
        w_alternate     = #f8f7f6
        w_base          = #fcfcfc
        w_button        = #eff0f1
        w_dark          = #888e93
        w_light         = #ffffff
        w_mid           = #c4c8cc
        w_midlight      = #f6f7f7
        w_shadow        = #474a4c
        w_tool_tip      = #fcfcfc
        w_window        = #eff0f1
        t_bright        = #ffffff
        t_button        = #31363b
        t_highlighted   = #fcfcfc
        t_placeholder   = #31363b
        t_text          = #31363b
        t_tool_tip      = #31363b
        t_window        = #31363b
        c_highlight     = #3daee9
        c_link          = #0057ae
        c_visited       = #452886
        """

        # Read
        palette = QApplication.palette()
        # Window
        w_alternate   = palette.alternateBase().color().name()
        w_base        = palette.base().color().name()
        w_button      = palette.button().color().name()
        w_dark        = palette.dark().color().name()
        w_light       = palette.light().color().name()
        w_mid         = palette.mid().color().name()
        w_midlight    = palette.midlight().color().name()
        w_shadow      = palette.shadow().color().name()
        w_tool_tip    = palette.toolTipBase().color().name()
        w_window      = palette.window().color().name()
        # Text
        t_bright      = palette.brightText().color().name()
        t_button      = palette.buttonText().color().name()
        t_highlighted = palette.highlightedText().color().name()
        t_placeholder = palette.placeholderText().color().name()
        t_text        = palette.text().color().name()
        t_tool_tip    = palette.toolTipText().color().name()
        t_window      = palette.windowText().color().name()
        # Color
        c_highlight   = palette.highlight().color().name()
        c_link        = palette.link().color().name()
        c_visited     = palette.linkVisited().color().name()
        # c_accent      = palette.accent().color().name() # qt6

        # Colors
        win = palette.window().color().getHsvF()
        hue = palette.highlight().color().getHsvF()
        m2 = +0.03; m3 = -0.03
        if win[2] > 0.5:    d2 = +0.20; d3 = -0.05 # Light Theme
        else:               d2 = +0.10; d3 = +0.10 # Dark Theme
        backdrop = QColor().fromHsvF( win[0], win[1] + 0,  win[2] - 0.05 ).name()
        menu     = QColor().fromHsvF( hue[0], win[1] + m2, win[2] + m3 ).name()
        # dim      = QColor().fromHsvF( hue[0], win[1] + d2, win[2] + d3 ).name()

        # Update
        self.channel_select.Set_Theme( t_button, w_dark )
        # Channel
        self.range_0.Set_Theme( t_window, backdrop )
        self.range_1.Set_Theme( t_window, backdrop )
        self.range_2.Set_Theme( t_window, backdrop )
        self.range_3.Set_Theme( t_window, backdrop )
        # Progressbar
        progress_style_sheet = self.Style_ProgressBar( c_highlight, "#00000000" )
        self.layout.progress_bar.setStyleSheet( progress_style_sheet )
        self.dialog.progress_bar.setStyleSheet( progress_style_sheet )

        # Style Sheets Layout
        self.layout.display_map.setStyleSheet( "#display_map{background-color: " + w_mid + ";}" )
        self.layout.page_channel.setStyleSheet( "#page_channel{background-color: " + backdrop + ";}" )
        self.layout.page_range.setStyleSheet( "#page_range{background-color: " + backdrop + ";}" )

        # Style Sheets Dialog
        self.dialog.scroll_area_contents_system.setStyleSheet( "#scroll_area_contents_system{background-color: " + menu + ";}" )
    def Style_ProgressBar( self, percentage, background ):
        style_sheet = str()
        style_sheet += "QProgressBar { background-color: " + background + "; border-radius: 0px; }"
        style_sheet += "QProgressBar::chunk { background-color: " + percentage + "; }"
        return style_sheet

    #endregion
    #region API

    # Display
    def API_Display_Image( self, qpixmap ):
        self.Display_Map( qpixmap, False )
    # Range
    def API_Input_Color_Index( self, color_index ):
        self.Range_Color_Index( color_index )
    # LUT
    def API_LUT_to_Image( self, list_url ):
        # Check
        report = True
        for url in list_url:
            reader = QImageReader( url )
            if reader.canRead() == False:
                report = False
                break
        # Operation
        if report == True:
            self.lut_list_url = list_url
            self.LUT_Menu()
        # Return
        return report

    #endregion
    #region UI LAYOUT DISPLAY

    def Display_Map( self, qpixmap, color_red ):
        if qpixmap != None:
            self.display_map.Update_Display( qpixmap )
            self.display_map.Update_Background( color_red )

    #endregion
    #region UI LAYOUT CHANNEL

    # Channels
    def Channel_Names( self, mode ):
        chan_0 = str()
        chan_1 = str()
        chan_2 = str()
        chan_3 = str()
        if mode in [ "A", "GRAY" ]:
            chan_0 = "Gray"
        if mode in [ "SRGB", "LRGB" ]:
            chan_0 = "RGB\nRed"
            chan_1 = "RGB\nGreen"
            chan_2 = "RGB\nBlue"
        if mode == "CMYK":
            chan_0 = "CMYK\nCyan"
            chan_1 = "CMYK\nMagenta"
            chan_2 = "CMYK\nYellow"
            chan_3 = "CMYK\nKey"
        if mode == "RYB":
            chan_0 = "RYB\nRed"
            chan_1 = "RYB\nYellow"
            chan_2 = "RYB\nBlue"
        if mode == "YUV":
            chan_0 = "YUV\nLuma"
            chan_1 = "YUV\nComp Blue"
            chan_2 = "YUV\nComp Red"
        if mode == "HSV":
            chan_0 = "HSV\nHue"
            chan_1 = "HSV\nSaturation"
            chan_2 = "HSV\nValue"
        if mode == "HSL":
            chan_0 = "HSL\nHue"
            chan_1 = "HSL\nSaturation"
            chan_2 = "HSL\nLightness"
        if mode == "HCY":
            chan_0 = "HCY\nHue"
            chan_1 = "HCY\nChroma"
            chan_2 = "HCY\nLuma"
        if mode == "ARD":
            chan_0 = "ARD\nAngle"
            chan_1 = "ARD\nRatio"
            chan_2 = "ARD\nDepth"
        if mode == "XYZ":
            chan_0 = "XYZ\nX"
            chan_1 = "XYZ\nY"
            chan_2 = "XYZ\nZ"
        if mode == "XYY":
            chan_0 = "XYY\nx"
            chan_1 = "XYY\ny"
            chan_2 = "XYY\nY"
        if mode == "LAB":
            chan_0 = "LAB\nL*"
            chan_1 = "LAB\nA*"
            chan_2 = "LAB\nB*"
        if mode == "LCH":
            chan_0 = "LCH\nLuminosity"
            chan_1 = "LCH\nChroma"
            chan_2 = "LCH\nHue"
        return chan_0, chan_1, chan_2, chan_3
    def Channel_Icons( self ):
        # Variables
        margin = 2
        height = self.layout.channel_select.height() - ( 2 * margin )
        # Construct List
        maps = list()
        for item in self.channel_data:
            # Variables
            render = item["render"]
            draw = render.scaled( int( height * 1.2 ), int( height ), Qt.KeepAspectRatio, Qt.FastTransformation )
            dw = draw.width()
            dh = draw.height()
            text = item["text"]
            # List construct
            maps.append( { "render" : draw, "width" : dw, "height" : dh, "text" : text } )
        self.channel_select.Update_Channel_Map( maps )
    def Channel_Index( self, channel_index ):
        self.channel_index = channel_index
        item = self.channel_data[ self.channel_index ]
        self.Display_Map( item["render"], item["cor"] )

    #endregion
    #region UI LAYOUT RANGE

    # Color
    def Range_Color_Read( self ):
        if self.pigmento_picker != None:
            color_index = self.pigmento_picker.API_Request_FG()
            self.Range_Color_Index( color_index )
    def Range_Color_Index( self, color_index ):
        # Range Display
        if self.color_space in [ "A", "GRAY" ]:
            self.range_0.Update_Color( color_index[ "gray_1" ] )
            self.range_1.Update_Color( None )
            self.range_2.Update_Color( None )
            self.range_3.Update_Color( None )
        if self.color_space == "SRGB":
            self.range_0.Update_Color( color_index[ "srgb_1" ] )
            self.range_1.Update_Color( color_index[ "srgb_2" ] )
            self.range_2.Update_Color( color_index[ "srgb_3" ] )
            self.range_3.Update_Color( None )
        if self.color_space == "LRGB":
            self.range_0.Update_Color( color_index[ "lrgb_1" ] )
            self.range_1.Update_Color( color_index[ "lrgb_2" ] )
            self.range_2.Update_Color( color_index[ "lrgb_3" ] )
            self.range_3.Update_Color( None )
        if self.color_space == "CMYK":
            self.range_0.Update_Color( color_index[ "cmyk_1" ] )
            self.range_1.Update_Color( color_index[ "cmyk_2" ] )
            self.range_2.Update_Color( color_index[ "cmyk_3" ] )
            self.range_3.Update_Color( color_index[ "cmyk_4" ] )
        if self.color_space == "RYB":
            self.range_0.Update_Color( color_index[ "ryb_1" ] )
            self.range_1.Update_Color( color_index[ "ryb_2" ] )
            self.range_2.Update_Color( color_index[ "ryb_3" ] )
            self.range_3.Update_Color( None )
        if self.color_space == "YUV":
            self.range_0.Update_Color( color_index[ "yuv_1" ] )
            self.range_1.Update_Color( color_index[ "yuv_2" ] )
            self.range_2.Update_Color( color_index[ "yuv_3" ] )
            self.range_3.Update_Color( None )
        if self.color_space == "HSV":
            self.range_0.Update_Color( color_index[ "hsv_1" ] )
            self.range_1.Update_Color( color_index[ "hsv_2" ] )
            self.range_2.Update_Color( color_index[ "hsv_3" ] )
            self.range_3.Update_Color( None )
        if self.color_space == "HSL":
            self.range_0.Update_Color( color_index[ "hsl_1" ] )
            self.range_1.Update_Color( color_index[ "hsl_2" ] )
            self.range_2.Update_Color( color_index[ "hsl_3" ] )
            self.range_3.Update_Color( None )
        if self.color_space == "HCY":
            self.range_0.Update_Color( color_index[ "hcy_1" ] )
            self.range_1.Update_Color( color_index[ "hcy_2" ] )
            self.range_2.Update_Color( color_index[ "hcy_3" ] )
            self.range_3.Update_Color( None )
        if self.color_space == "ARD":
            self.range_0.Update_Color( color_index[ "ard_1" ] )
            self.range_1.Update_Color( color_index[ "ard_2" ] )
            self.range_2.Update_Color( color_index[ "ard_3" ] )
            self.range_3.Update_Color( None )
        if self.color_space == "XYZ":
            self.range_0.Update_Color( color_index[ "xyz_1" ] )
            self.range_1.Update_Color( color_index[ "xyz_2" ] )
            self.range_2.Update_Color( color_index[ "xyz_3" ] )
            self.range_3.Update_Color( None )
        if self.color_space == "XYY":
            self.range_0.Update_Color( color_index[ "xyy_1" ] )
            self.range_1.Update_Color( color_index[ "xyy_2" ] )
            self.range_2.Update_Color( color_index[ "xyy_3" ] )
            self.range_3.Update_Color( None )
        if self.color_space == "LAB":
            self.range_0.Update_Color( color_index[ "lab_1" ] )
            self.range_1.Update_Color( color_index[ "lab_2" ] )
            self.range_2.Update_Color( color_index[ "lab_3" ] )
            self.range_3.Update_Color( None )
        if self.color_space == "LCH":
            self.range_0.Update_Color( color_index[ "lch_1" ] )
            self.range_1.Update_Color( color_index[ "lch_2" ] )
            self.range_2.Update_Color( color_index[ "lch_3" ] )
            self.range_3.Update_Color( None )

    # Range 1
    def Range_0_PA( self, value ):
        self.range_0_pa = value
    def Range_0_PB( self, value ):
        self.range_0_pb = value
    def Range_0_PC( self, value ):
        self.range_0_pc = value
    def Range_0_PD( self, value ):
        self.range_0_pd = value
    # Range 2
    def Range_1_PA( self, value ):
        self.range_1_pa = value
    def Range_1_PB( self, value ):
        self.range_1_pb = value
    def Range_1_PC( self, value ):
        self.range_1_pc = value
    def Range_1_PD( self, value ):
        self.range_1_pd = value
    # Range 3
    def Range_2_PA( self, value ):
        self.range_2_pa = value
    def Range_2_PB( self, value ):
        self.range_2_pb = value
    def Range_2_PC( self, value ):
        self.range_2_pc = value
    def Range_2_PD( self, value ):
        self.range_2_pd = value
    # Range 4
    def Range_3_PA( self, value ):
        self.range_3_pa = value
    def Range_3_PB( self, value ):
        self.range_3_pb = value
    def Range_3_PC( self, value ):
        self.range_3_pc = value
    def Range_3_PD( self, value ):
        self.range_3_pd = value

    #endregion
    #region UI LAYOUT LUT

    # Menu
    def LUT_Menu( self ):
        self.dlut.show()
    def LUT_File_URL( self, url ):
        if os.path.isfile( url ) == True:
            self.lut_file_url = url
            Kritarc_Write( DOCKER_SAMPLER, "lut_file_url", self.lut_file_url )
    def LUT_File_Dialog( self ):
        lut_file_url = self.Open_File( "Select LUT file", os.path.dirname( self.lut_file_url ), "cube" )
        if os.path.isfile( lut_file_url ) == True:
            self.lut_file_url = lut_file_url
            self.dlut.lut_file_url.setText( self.lut_file_url )
            Kritarc_Write( DOCKER_SAMPLER, "lut_file_url", self.lut_file_url )
    def LUT_Deposit_URL( self, url ):
        if os.path.isdir( url ) == True:
            self.lut_deposit_url = url
            Kritarc_Write( DOCKER_SAMPLER, "lut_deposit_url", self.lut_deposit_url )
    def LUT_Deposit_Dialog( self ):
        lut_deposit_url = self.Open_Directory( "Select Destination Path", self.lut_deposit_url )
        if os.path.isdir( lut_deposit_url ) == True:
            self.lut_deposit_url = lut_deposit_url
            self.dlut.lut_deposit_url.setText( self.lut_deposit_url )
            Kritarc_Write( DOCKER_SAMPLER, "lut_deposit_url", self.lut_deposit_url )
    def LUT_Accept( self ):
        self.dlut.close()
        self.LUT_to_Image( self.lut_list_url )
        self.lut_list_url.clear()
    def LUT_Abort( self ):
        self.dlut.close()
        self.lut_list_url.clear()

    # Process
    def LUT_to_Image( self, lut_list_url ):
        if self.lut_file_url != None:
            lut, size, dmin, dmax = self.LUT_Read( self.lut_file_url )
            # Length
            len_lut = len( lut )
            # Axis
            axis = list()
            for i in range( 0, size ):
                value = i / ( size - 1 )
                axis.append( value )
            # Apply LUT
            if lut != None:
                for image_url in lut_list_url:
                    qimage = QImage( image_url )
                    if qimage.isNull() == False:
                        # Dimension
                        width = qimage.width()
                        height = qimage.height()
                        # Progress Bar
                        self.ProgressBar_Maximum( height )
                        self.ProgressBar_Value( 1 )
                        # Cycle Pixels
                        for y in range( 0, height ):
                            # Progress Bar
                            y1 = y + 1
                            percent = int( ( y / height ) * 100 )
                            if ( percent % 2 ) == 0:
                                self.ProgressBar_Value( y1 )
                                QApplication.processEvents()
                            # Pixels
                            for x in range( 0, width ):
                                # Read
                                pixel = qimage.pixelColor( x, y )
                                pr = pixel.redF()
                                pg = pixel.greenF()
                                pb = pixel.blueF()
                                pa = pixel.alphaF()
                                # Convert
                                srgb = self.convert.lut_to_pixel( lut, len_lut, axis, size, dmin, dmax, pr, pg, pb )
                                pixel.setRgbF( srgb[0], srgb[1], srgb[2], pa )
                                qimage.setPixelColor( x, y, pixel )
                        # Display
                        qpixmap = QPixmap().fromImage( qimage )
                        self.display_map.Update_Display( qpixmap )
                        # Porgess Bar
                        self.ProgressBar_Value( height )
                        self.ProgressBar_Value( 0 )
                        self.ProgressBar_Maximum( 1 )
                        # Save to Deposit
                        if os.path.isdir( self.lut_deposit_url ) == True:
                            basename = os.path.basename( image_url ).split( "." )[0]
                            lut_tag = os.path.basename( self.lut_file_url ).split( "." )[0]
                            destination = os.path.normpath( os.path.join( self.lut_deposit_url, f"{ basename }_{ lut_tag }.png" ) )
                            qpixmap.save( destination )
                    # Update UI
                    QApplication.processEvents()
                    del qpixmap
    def LUT_Read( self, url ):
        # Variables
        title = None
        lut_3d_size = None
        domain_min = None
        domain_max = None
        lut = list()
        # Cycle
        with open( url, "r", encoding="utf-8" ) as lut_file:
            # Read
            data = lut_file.readlines()
            # Parse
            for line in data:
                if line.startswith( "#" ) == False:
                    if title == None and line.startswith( "TITLE " ):
                        line = line.replace( "TITLE ", "" )
                        title = str( line )
                    elif lut_3d_size == None and line.startswith( "LUT_3D_SIZE " ):
                        line = line.replace( "LUT_3D_SIZE ", "" )
                        lut_3d_size = int( line )
                    elif domain_min == None and line.startswith( "DOMAIN_MIN " ):
                        try:
                            line = line.replace( "DOMAIN_MIN ", "" )
                            split = line.split()
                            domain_min = [ float( split[0] ), float( split[1] ), float( split[2] ) ]
                        except:
                            domain_min = None
                    elif domain_max == None and line.startswith( "DOMAIN_MAX " ):
                        try:
                            line = line.replace( "DOMAIN_MAX ", "" )
                            split = line.split()
                            domain_max = [ float( split[0] ), float( split[1] ), float( split[2] ) ]
                        except:
                            domain_max = None
                    elif line.startswith( "#" ) == False:
                        try:
                            split = line.split()
                            vector = [ float( split[0] ), float( split[1] ), float( split[2] ) ]
                            lut.append( vector )
                        except:
                            pass
        # Confirm
        if len( lut ) != lut_3d_size**3:
            lut = None
        # Return
        return lut, lut_3d_size, domain_min, domain_max

    #endregion
    #region UI LAYOUT FOOTER

    # Workflow
    def Color_Space( self, color_space ):
        # Variables
        self.color_space = color_space

        # Range
        bnw = [
            [ 0.0, 0.0, 0.0 ],
            [ 0.5, 0.5, 0.5 ],
            [ 1.0, 1.0, 1.0 ],
            ]
        hue = [
            [ 1.0, 0.0, 0.0 ],
            [ 1.0, 1.0, 0.0 ],
            [ 0.0, 1.0, 0.0 ],
            [ 0.0, 1.0, 1.0 ],
            [ 0.0, 0.0, 1.0 ],
            [ 1.0, 0.0, 1.0 ],
            [ 1.0, 0.0, 0.0 ],
            ]
        # User Interface
        if   color_space in [ "A", "GRAY" ]:
            range_0 = bnw
            range_1 = None
            range_2 = None
            range_3 = None
        elif color_space == "SRGB":
            range_0 = [
                [ 0.0, 0.0, 0.0 ],
                [ 1.0, 0.0, 0.0 ],
                ]
            range_1 = [
                [ 0.0, 0.0, 0.0 ],
                [ 0.0, 1.0, 0.0 ],
                ]
            range_2 = [
                [ 0.0, 0.0, 0.0 ],
                [ 0.0, 0.0, 1.0 ],
                ]
            range_3 = None
        elif color_space == "LRGB":
            range_0 = [
                [ 0.0, 0.0, 0.0 ],
                [ 0.54, 0.0, 0.0 ],
                [ 0.74, 0.0, 0.0 ],
                [ 0.88, 0.0, 0.0 ],
                [ 1.0, 0.0, 0.0 ],
                ]
            range_1 = [
                [ 0.0, 0.0, 0.0 ],
                [ 0.0, 0.54, 0.0 ],
                [ 0.0, 0.74, 0.0 ],
                [ 0.0, 0.88, 0.0 ],
                [ 0.0, 1.0, 0.0 ],
                ]
            range_2 = [
                [ 0.0, 0.0, 0.0 ],
                [ 0.0, 0.0, 0.54 ],
                [ 0.0, 0.0, 0.74 ],
                [ 0.0, 0.0, 0.88 ],
                [ 0.0, 0.0, 1.0 ],
                ]
            range_3 = None
        elif color_space == "CMYK":
            range_0 = [
                [ 0.0, 0.0, 0.0 ],
                [ 0.0, 1.0, 1.0 ],
                ]
            range_1 = [
                [ 0.0, 0.0, 0.0 ],
                [ 1.0, 0.0, 1.0 ],
                ]
            range_2 = [
                [ 0.0, 0.0, 0.0 ],
                [ 1.0, 1.0, 0.0 ],
                ]
            range_3 = [
                [ 1.0, 1.0, 1.0 ],
                [ 0.0, 0.0, 0.0 ],
                ]
        elif color_space == "RYB":
            range_0 = [
                [ 0.0, 0.0, 0.0 ],
                [ 1.0, 0.0, 0.0 ],
                ]
            range_1 = [
                [ 0.0, 0.0, 0.0 ],
                [ 1.0, 1.0, 0.0 ],
                ]
            range_2 = [
                [ 0.0, 0.0, 0.0 ],
                [ 0.0, 0.0, 1.0 ],
                ]
            range_3 = None
        elif color_space == "YUV":
            range_0 = bnw
            range_1 = [
                [ 0.50, 0.59, 0.00 ],
                [ 0.50, 0.55, 0.04 ],
                [ 0.50, 0.50, 0.50 ],
                [ 0.50, 0.45, 0.96 ],
                [ 0.50, 0.41, 1.00 ],
                ]
            range_2 = [
                [ 0.00, 0.73, 0.50 ],
                [ 0.11, 0.62, 0.50 ],
                [ 0.50, 0.50, 0.50 ],
                [ 0.89, 0.34, 0.96 ],
                [ 1.00, 0.27, 0.50 ],
                ]
            range_3 = None
        elif color_space == "HSV":
            range_0 = hue
            range_1 = [
                [ 1.0, 1.0, 1.0 ],
                [ 1.0, 0.0, 0.0 ],
                ]
            range_2 = bnw
            range_3 = None
        elif color_space == "HSL":
            range_0 = hue
            range_1 = [
                [ 0.5, 0.5, 0.5 ],
                [ 1.0, 0.0, 0.0 ],
                ]
            range_2 = [
                [ 0.0, 0.0, 0.0 ],
                [ 1.0, 0.0, 0.0 ],
                [ 1.0, 1.0, 1.0 ],
                ]
            range_3 = None
        elif color_space == "HCY":
            range_0 = hue
            range_1 = [
                [ 0.21, 0.21, 0.21 ],
                [ 0.41, 0.16, 0.16 ],
                [ 0.61, 0.11, 0.11 ],
                [ 0.80, 0.05, 0.05 ],
                [ 1.00, 0.00, 0.00 ],
                ]
            range_2 = bnw
            range_3 = None
        elif color_space == "ARD":
            range_0 = hue
            range_1 = [
                [ 0.33, 0.33, 0.33 ],
                [ 0.50, 0.25, 0.25 ],
                [ 0.67, 0.17, 0.17 ],
                [ 0.83, 0.08, 0.08 ],
                [ 1.00, 0.00, 0.00 ],
                ]
            range_2 = bnw
            range_3 = None
        elif color_space == "XYZ":
            range_0 = [
                [ 0.00, 0.00, 0.00 ],
                [ 0.91, 0.00, 0.12 ],
                [ 1.00, 0.00, 0.18 ],
                [ 1.00, 0.00, 0.23 ],
                [ 1.00, 0.00, 0.26 ],
                ]
            range_1 = [
                [ 0.00, 0.00, 0.00 ],
                [ 0.00, 0.71, 0.00 ],
                [ 0.00, 0.97, 0.00 ],
                [ 0.00, 1.00, 0.00 ],
                [ 0.00, 1.00, 0.00 ],
                ]
            range_2 = [
                [ 0.00, 0.00, 0.00 ],
                [ 0.00, 0.10, 0.55 ],
                [ 0.00, 0.16, 0.75 ],
                [ 0.00, 0.19, 0.90 ],
                [ 0.00, 0.23, 1.00 ],
                ]
            range_3 = None
        elif color_space == "XYY":
            range_0 = [
                [ 0.00, 0.68, 0.67 ],
                [ 0.27, 0.54, 0.54 ],
                [ 0.83, 0.33, 0.33 ],
                [ 1.00, 0.00, 0.00 ],
                [ 1.00, 0.00, 0.00 ],
                ]
            range_1 = [
                [ 1.00, 0.00, 1.00 ],
                [ 1.00, 0.00, 0.32 ],
                [ 0.79, 0.40, 0.00 ],
                [ 0.60, 0.50, 0.00 ],
                [ 0.46, 0.55, 0.00 ],
                ]
            range_2 = [
                [ 0.00, 0.00, 0.00 ],
                [ 0.54, 0.54, 0.54 ],
                [ 0.74, 0.74, 0.74 ],
                [ 0.88, 0.88, 0.88 ],
                [ 1.00, 1.00, 1.00 ],
                ]
            range_3 = None
        elif color_space == "LAB":
            range_0 = [
                [ 0.00, 0.00, 0.00 ],
                [ 0.23, 0.23, 0.23 ],
                [ 0.47, 0.47, 0.47 ],
                [ 0.72, 0.72, 0.72 ],
                [ 1.00, 1.00, 1.26 ],
                ]
            range_1 = [
                [ 0.00, 0.64, 0.45 ],
                [ 0.00, 0.60, 0.46 ],
                [ 0.47, 0.47, 0.47 ],
                [ 1.00, 0.00, 0.49 ],
                [ 1.00, 0.00, 0.53 ],
                ]
            range_2 = [
                [ 0.00, 0.52, 1.00 ],
                [ 0.00, 0.49, 0.80 ],
                [ 0.47, 0.47, 0.47 ],
                [ 0.55, 0.46, 0.10 ],
                [ 0.57, 0.46, 0.00 ],
                ]
            range_3 = None
        elif color_space == "LCH":
            range_0 = [
                [ 0.00, 0.00, 0.00 ],
                [ 0.23, 0.23, 0.23 ],
                [ 0.47, 0.47, 0.47 ],
                [ 0.72, 0.72, 0.72 ],
                [ 1.00, 1.00, 1.00 ],
                ]
            range_1 = [
                [ 0.47, 0.47, 0.47 ],
                [ 0.67, 0.39, 0.32 ],
                [ 0.83, 0.28, 0.17 ],
                [ 0.96, 0.00, 0.00 ],
                [ 1.00, 0.00, 0.00 ],
                ]
            range_2 = [
                [ 1.00, 0.00, 0.53 ],
                [ 1.00, 0.00, 0.00 ],
                [ 0.00, 0.60, 0.00 ],
                [ 0.00, 0.64, 0.45 ],
                [ 0.00, 0.64, 1.00 ],
                [ 0.94, 0.00, 1.00 ],
                [ 1.00, 0.00, 0.53 ],
                ]
            range_3 = None
        # Mode
        hue = ( "HSV", "HSL", "HCY", "ARD" )
        if color_space in hue:      self.range_0.Update_Mode( "CIRCULAR" )
        else:                       self.range_0.Update_Mode( "LINEAR" )
        self.range_1.Update_Mode( "LINEAR" )
        if color_space == "LCH":    self.range_2.Update_Mode( "CIRCULAR" )
        else:                       self.range_2.Update_Mode( "LINEAR" )
        self.range_3.Update_Mode( "LINEAR" )
        # Display Gradients
        self.range_0.Update_Gradient( range_0 )
        self.range_1.Update_Gradient( range_1 )
        self.range_2.Update_Gradient( range_2 )
        self.range_3.Update_Gradient( range_3 )
        # Color
        self.Range_Color_Read()
        Kritarc_Write( DOCKER_SAMPLER, "color_space", self.color_space )
    def Split_Method( self, split_method ):
        # Variables
        self.split_method = split_method
        qpixmap = self.qpixmap_logo
        boolean = False
        # Mode
        if self.split_method == "CHANNEL":
            self.layout.stacked_widget.setCurrentIndex( 0 )
            self.layout.stacked_widget.setMaximumHeight( 120 )
            try:
                qpixmap = self.channel_data[self.channel_index]["render"]
                boolean = True
            except:
                pass
        if self.split_method == "RANGE":
            self.layout.stacked_widget.setCurrentIndex( 1 )
            self.layout.stacked_widget.setMaximumHeight( 120 )
            try:
                qpixmap = self.range_data[0]["render"]
                boolean = True
            except:
                pass
            self.Range_Color_Read()
        # User Interface
        self.Display_Map( qpixmap, False )
        self.Widget_Insert( boolean )
        self.Size_Update()
        # Save
        Kritarc_Write( DOCKER_SAMPLER, "split_method", self.split_method )

    # Masks
    def Mask_Generate( self ):
        if ( self.kcanvas != None ) and ( self.kview != None ):
            # Time Watcher
            self.time = QtCore.QDateTime.currentDateTimeUtc()

            # User Interface
            self.Widgets_Enabled( False )

            # Active Document
            ki = Krita.instance()
            ad = ki.activeDocument()
            # Document Color
            d_cm = ad.colorModel()
            d_cd = ad.colorDepth()
            d_cp = ad.colorProfile()

            # Color Model
            if   d_cm in [ "A", "GRAYA" ]:  cmodel = "GRAY"
            elif d_cm == "RGBA":            cmodel = "SRGB"
            elif d_cm == "CMYKA":           cmodel = "CMYK"
            elif d_cm == "YCbCrA":          cmodel = "YUV"
            elif d_cm == "XYZA":            cmodel = "XYZ"
            elif d_cm == "LABA":            cmodel = "LAB"
            else:                           cmodel = "SRGB"

            # Place Text
            self.layout.label.setText( "" )
            Message_Float( "GENERATE", self.split_method, "color-adjustment-mode-channels" )
            QApplication.processEvents()

            # Size
            width = ad.width()
            height = ad.height()

            # Depth Constants
            depth = 255
            if d_cd == "U16":   depth = 65535
            elif d_cd == "F16": depth = 65535
            elif d_cd == "F32": depth = 4294836225
            k = 255

            # Source
            ss = ad.selection()
            if ss == None:
                dx = 0
                dy = 0
                dw = width
                dh = height
            else:
                dx = ss.x()
                dy = ss.y()
                dw = ss.width()
                dh = ss.height()

            # Pixel Data Colors
            byte_array = ad.pixelData( dx, dy, dw, dh )
            num_array = self.analyse.Bytes_to_Integer( byte_array, d_cd )

            # Pixel Data Selection
            if ss == None:
                num_ss = [ depth ] * len( num_array )
            else:
                byte_ss = ss.pixelData( dx, dy, dw, dh )
                num_ss = self.analyse.Bytes_to_Integer( byte_ss, None )

            # Run Worker
            run_variables = ( cmodel, d_cd, depth, k, dx, dy, dw, dh, num_array, num_ss )
            self.Samples_Process( run_variables )
        else:
            Message_Warnning( "ERROR", "Canvas not Found" )
        # Progress bar
        self.ProgressBar_Value( 0 )
    # Worker
    def Samples_Process( self, run_variables ):
        thread = True
        if thread == False: self.Samples_Single( run_variables )
        if thread == True:  self.Samples_Thread( run_variables )
    def Samples_Connect( self ):
        self.samples_worker = Worker_Samples()
        self.samples_worker.SIGNAL_FINISH.connect( self.Samples_Finish )
    def Samples_Single( self, run_variables ):
        self.Samples_Connect()
        self.samples_worker.run( self, self.split_method, run_variables, False )
    def Samples_Thread( self, run_variables ):
        # Thread
        self.samples_qthread = QtCore.QThread()
        # Worker
        self.Samples_Connect()
        self.samples_worker.moveToThread( self.samples_qthread )
        # Thread
        self.samples_qthread.started.connect( lambda : self.samples_worker.run( self, self.split_method, run_variables, True ) )
        self.samples_qthread.start()
    def Samples_Finish( self ):
        # User Interface
        self.ProgressBar_Value( 0 )
        self.ProgressBar_Maximum( 1 )
        self.Widgets_Enabled( True )
        self.update()
        # Time Watcher
        end = QtCore.QDateTime.currentDateTimeUtc()
        delta = self.time.msecsTo( end )
        time = QTime( 0,0 ).addMSecs( delta )
        Message_Log( self.split_method, f"{ time.toString( 'hh:mm:ss.zzz' ) }" )
        # Sound
        QApplication.beep()

    # Mask
    def Mask_Insert( self ):
        # num_array - list of integer numbers, represents each pixels channels. RGB U8 > [ B,G,R,A, B,G,R,A, B,G,R,A, ... ]

        # Variables
        item = None
        len_cd = len( self.channel_data )
        len_rd = len( self.range_data )
        if len_cd > 0 or len_rd > 0:
            # Item
            if self.split_method == "CHANNEL":  item = self.channel_data[ self.channel_index ]
            if self.split_method == "RANGE":    item = self.range_data[0]
            if item != None:
                # Variables
                num_array = item["map"]
                px = item["dx"]
                py = item["dy"]
                width = item["dw"]
                height = item["dh"]

                # Apply Map
                if ( self.kcanvas != None ) and ( self.kview != None ):
                    # Variables
                    ki = Krita.instance()
                    ad = ki.activeDocument()
                    nt = ad.activeNode().type()

                    # Place selection on good parent node
                    if nt in [ "paintlayer", "grouplayer" ]:
                        # Deselect all
                        ki.action( "deselect" ).trigger()
                        # Place Text
                        Message_Float( "INSERT", "Selection", "local-selection-active" )
                        # Selection
                        sel = Selection()
                        sel.setPixelData( bytes( num_array ), px, py, width, height )
                        ad.setSelection( sel )
                        # Document Response Time
                        ad.waitForDone()
                        ad.refreshProjection()
                        # Make Selection
                        ki.action( "add_new_selection_mask" ).trigger()
                        ki.action( "invert_selection" ).trigger()
                        ki.action( "invert_selection" ).trigger()
                    else:
                        Message_Float( "ERROR", "Invalid active layer to Insert", self.qicon_insert )
                else:
                    Message_Warnning( "ERROR", "Canvas not found" )
        else:
            Message_Float( "ERROR", "Map data not found", self.qicon_insert )
    def Mask_Clean( self ):
        # Display
        self.Display_Map( self.qpixmap_logo, False )
        # Mode
        if self.split_method == "CHANNEL":  self.channel_data = list(); self.channel_select.Update_Channel_Map( None )
        if self.split_method == "RANGE":    self.range_data = list()
        # Footer
        self.Widget_Insert( False )

    # Settings
    def Menu_Settings( self ):
        # Display
        self.dialog.show()
        # Resize Geometry
        qmw = Krita.instance().activeWindow().qwindow()
        px = qmw.x()
        py = qmw.y()
        w2 = qmw.width() * 0.5
        h2 = qmw.height() * 0.5
        size = 500
        self.dialog.setGeometry( int( px + w2 - size * 0.5 ), int( py + h2 - size * 0.5 ), int( size ), int( size ) )

    #endregion
    #region UI DIALOG

    # Colors Spaces
    def CS_Luminosity( self, cs_luminosity ):
        self.cs_luminosity = cs_luminosity
        self.convert.Set_Luminosity( self.cs_luminosity )
        Kritarc_Write( DOCKER_SAMPLER, "cs_luminosity", self.cs_luminosity )
    def CS_Matrix( self, cs_matrix ):
        self.cs_matrix = cs_matrix
        self.convert.Set_Matrix( self.cs_matrix )
        Kritarc_Write( DOCKER_SAMPLER, "cs_matrix", self.cs_matrix )

    # Maps
    def TIC_Display( self, boolean ):
        self.tic_display = boolean
        Kritarc_Write( DOCKER_SAMPLER, "tic_display", self.tic_display )
    def TIC_Invert( self, boolean ):
        self.tic_invert = boolean
        Kritarc_Write( DOCKER_SAMPLER, "tic_invert", self.tic_invert )
    def TIC_Value( self, value ):
        self.tic_value = value
        Kritarc_Write( DOCKER_SAMPLER, "tic_value", self.tic_value )
    def CMYK_Invert( self, boolean ):
        self.cmyk_invert = boolean
        Kritarc_Write( DOCKER_SAMPLER, "cmyk_invert", self.cmyk_invert )

    # Help
    def Menu_Manual( self ):
        url = "https://github.com/EyeOdin/Pigment.O/wiki"
        webbrowser.open_new( url )
    def Menu_License( self ):
        url = "https://github.com/EyeOdin/Pigment.O/blob/master/LICENSE"
        webbrowser.open_new( url )

    #endregion
    #region Notifier

    # Notifier
    def Application_Closing( self ):
        pass
    def Configuration_Changed( self ):
        pass
    def Image_Closed( self ):
        pass
    def Image_Created( self ):
        pass
    def Image_Saved( self ):
        pass
    def View_Closed( self ):
        pass
    def View_Created( self ):
        pass
    def Window_Created( self ):
        # Module
        ki = Krita.instance()
        self.window = ki.activeWindow()
        # Signals
        self.window.activeViewChanged.connect( self.View_Changed )
        self.window.themeChanged.connect( self.Theme_Changed )
        self.window.windowClosed.connect( self.Window_Closed )
    def Window_IsBeingCreated( self ):
        pass

    # Window
    def View_Changed( self ):
        pass
    def Theme_Changed( self ):
        self.Style_Icon()
        self.Style_Theme()
    def Window_Closed( self ):
        pass

    #endregion
    #region Widget Events 

    def showEvent( self, event ):
        self.Import_Picker()
        self.Size_Update()
    def resizeEvent( self, event ):
        # self.Size_Print()
        self.Size_Update()
    def enterEvent( self, event ):
        self.Range_Color_Read()
    def leaveEvent( self, event ):
        pass
    def closeEvent( self, event ):
        pass

    def eventFilter( self, source, event ):
        # Event
        et = event.type()
        modifier_all = QtCore.Qt.ShiftModifier | QtCore.Qt.ControlModifier | QtCore.Qt.AltModifier
        # Settings
        if ( et == QEvent.MouseButtonPress and event.modifiers() == modifier_all and source is self.layout.settings ):
            self.Size_Standard()
        return super().eventFilter( source, event )

    def canvasChanged( self, canvas ):
        self.kcanvas = canvas
        if canvas == None: self.kview = None
        else:              self.kview = canvas.view()

    #endregion
    #region Notes

    """
    # Label Message
    self.layout.label.setText( "message" )

    # Pop Up Message
    QMessageBox.information( QWidget(), i18n( "Warnning" ), i18n( "message" ) )

    # Log Viewer Message
    QtCore.qDebug( f"value = { value }" )
    QtCore.qDebug( "message" )
    QtCore.qWarning( "message" )
    QtCore.qCritical( "message" )

    # qimage = QImage( byte_array, width, height, QImage.Format_RGBA8888 )
    """

    #endregion
 
class Worker_Samples( QObject ):
    SIGNAL_FINISH = QtCore.pyqtSignal()

    def run( self, source, split_method, run_variables, thread ):
        if thread == True:  source.samples_qthread.setPriority( QThread.HighestPriority )
        if split_method == "CHANNEL":   Run_Channel( source, *run_variables )
        elif split_method == "RANGE":   Run_Range( source, *run_variables )
        if thread == True:  source.samples_qthread.quit()
        self.SIGNAL_FINISH.emit()
# Cycles
def Run_Channel( self, cmodel, d_cd, depth, k, dx, dy, dw, dh, num_array, num_ss ):
    # Variables
    index = 0
    c0 = 0.75
    c1 = 0.25
    c2 = 0.25
    cor = False
    hue_rgb = [ "HSV", "HSL", "HCY", "ARD" ]
    hue_xyz = [ "LCH" ]
    numbers = list()

    # Channels
    channels = 3
    if self.color_space in [ "A", "GRAY" ]: channels = 1
    if self.color_space == "CMYK":          channels = 4
    alpha = 1
    number = channels + alpha + int( self.tic_display )
    self.channel_select.Update_Channel_Number( number )

    # Item Selection
    try:    previous = Limit_Range( self.channel_index, 0, number - 1 )
    except: previous = 0

    # Channel names
    chan_0, chan_1, chan_2, chan_3 = self.Channel_Names( self.color_space )

    # Lists Render and Maps
    byte_0_r = list(); byte_0_m = list()
    byte_1_r = list(); byte_1_m = list()
    byte_2_r = list(); byte_2_m = list()
    byte_3_r = list(); byte_3_m = list()
    byte_t_r = list(); byte_t_m = list()
    byte_a_r = list(); byte_a_m = list()

    # Progress bar
    self.ProgressBar_Maximum( dh )
    self.ProgressBar_Value( 1 )
    QApplication.processEvents()

    # Cycle
    for y in range( 0, dh ):
        # Progress Bar
        y1 = y + 1
        percent = int( ( y / dh ) * 100 )
        if ( percent % 2 ) == 0:
            self.ProgressBar_Value( y1 )
            QApplication.processEvents()
        if numbers == None:break

        # Pixel
        for x in range( 0, dw ):
            # Read Byte
            numbers = self.analyse.Numbers_on_Pixel( cmodel, d_cd, index, num_array )
            if numbers == None:break
            ssi = num_ss[index] / depth

            # Convert
            if cmodel in [ "A", "GRAY" ]:
                # Variables
                n0 = numbers[0] / depth
                na = numbers[1] / depth
                # Convert
                conv = self.convert.color_convert( cmodel, self.color_space, [ n0 ] )
                # Variables
                cmyk = self.convert.srgb_to_cmyk( n0, n0, n0, None )
                bw = 1 - cmyk[3]
            elif cmodel == "SRGB":
                # Variables
                n0 = numbers[0] / depth
                n1 = numbers[1] / depth
                n2 = numbers[2] / depth
                na = numbers[3] / depth
                # Convert
                conv = self.convert.color_convert( cmodel, self.color_space, [ n0, n1, n2 ] )
                # Variables
                cmyk = self.convert.srgb_to_cmyk( n0, n1, n2, None )
                bw = 1 - cmyk[3]
            elif cmodel == "CMYK":
                # Variables
                n0 = numbers[0] / depth
                n1 = numbers[1] / depth
                n2 = numbers[2] / depth
                n3 = numbers[3] / depth
                na = numbers[4] / depth
                # Convert
                conv = self.convert.color_convert( cmodel, self.color_space, [ n0, n1, n2, n3 ] )
                # Variables
                cmyk = [ n0, n1, n2, n3 ]
                bw = 1 - n3

            # Length
            length = len( conv )

            # Channels
            occ = na * ssi
            if length == 1:
                s0 = int( Limit_Float( conv[0] ) * occ * k )
            elif length == 3:
                if self.color_space in hue_rgb:
                    hrgb = self.convert.hue_to_srgb( conv[0] )
                    hue0 = int( hrgb[0] * occ * k )
                    hue1 = int( hrgb[1] * occ * k )
                    hue2 = int( hrgb[2] * occ * k )
                if self.color_space in hue_xyz:
                    srgb = self.convert.lch_to_srgb( conv[0], conv[1], conv[2] )
                    hue = self.convert.srgb_to_hue( srgb[0], srgb[1], srgb[2] )
                    hrgb = self.convert.hue_to_srgb( hue )
                    hue0 = int( hrgb[0] * occ * k )
                    hue1 = int( hrgb[1] * occ * k )
                    hue2 = int( hrgb[2] * occ * k )
                s0 = int( Limit_Float( conv[0] ) * occ * k )
                s1 = int( Limit_Float( conv[1] ) * occ * k )
                s2 = int( Limit_Float( conv[2] ) * occ * k )
            elif length == 4:
                s0 = int( Limit_Float( conv[0] ) * occ * k )
                s1 = int( Limit_Float( conv[1] ) * occ * k )
                s2 = int( Limit_Float( conv[2] ) * occ * k )
                if self.cmyk_invert == True:
                    s3 = int( Limit_Float( 1 - conv[3] ) * occ * k )
                else:
                    s3 = int( Limit_Float( conv[3] ) * occ * k )
            # Total Ink Cove_rage
            if self.tic_display == True:
                tic = self.convert.cmyk_to_sum( cmyk[0], cmyk[1], cmyk[2], cmyk[3] )
                t0, t1, t2, tw, cor = self.analyse.Total_Ink_Coverage( self.tic_invert, tic * 100, self.tic_value, c0, c1, c2, bw, cor )
                t0 = int( t0 * occ * k )
                t1 = int( t1 * occ * k )
                t2 = int( t2 * occ * k )
                tw = int( tw * occ * k )
            # Alpha
            na = int( na * occ * k )

            # Images
            if length == 1:
                byte_0_r.extend( [ s0, s0, s0, na ] )
            elif length == 3:
                if self.color_space in hue_rgb:
                    byte_0_r.extend( [ hue0, hue1, hue2, na ] )
                else:
                    byte_0_r.extend( [ s0, s0, s0, na ] )
                byte_1_r.extend( [ s1, s1, s1, na ] )
                if self.color_space in hue_xyz:
                    byte_2_r.extend( [ hue0, hue1, hue2, na ] )
                else:
                    byte_2_r.extend( [ s2, s2, s2, na ] )
            elif length == 4:
                byte_0_r.extend( [ s0, s0, s0, na ] )
                byte_1_r.extend( [ s1, s1, s1, na ] )
                byte_2_r.extend( [ s2, s2, s2, na ] )
                byte_3_r.extend( [ s3, s3, s3, na ] )
            byte_a_r.extend( [ na, na, na, k ] )
            if self.tic_display == True:
                byte_t_r.extend( [ t0, t1, t2, na ] )

            # Maps
            if length == 1:
                byte_0_m.append( s0 )
            elif length == 3:
                byte_0_m.append( s0 )
                byte_1_m.append( s1 )
                byte_2_m.append( s2 )
            elif length == 4:
                byte_0_m.append( s0 )
                byte_1_m.append( s1 )
                byte_2_m.append( s2 )
                byte_3_m.append( s3 )
            byte_a_m.append( na )
            if self.tic_display == True:
                byte_t_m.append( tw )

            # Cycle
            index += 1

    # Progress Bar
    self.ProgressBar_Value( dh )
    QApplication.processEvents()

    # Check
    if numbers != None and len( byte_0_m ) > 0:
        # QPixmap
        qimage_format = QImage.Format_RGBA8888
        if length >= 1: pix_0_r = QPixmap().fromImage( QImage( bytes( byte_0_r ), dw, dh, qimage_format ) )
        if length >= 2: pix_1_r = QPixmap().fromImage( QImage( bytes( byte_1_r ), dw, dh, qimage_format ) )
        if length >= 3: pix_2_r = QPixmap().fromImage( QImage( bytes( byte_2_r ), dw, dh, qimage_format ) )
        if length >= 4: pix_3_r = QPixmap().fromImage( QImage( bytes( byte_3_r ), dw, dh, qimage_format ) )
        pix_a_r = QPixmap().fromImage( QImage( bytes( byte_a_r ), dw, dh, qimage_format ) )
        if self.tic_display == True: pix_t_r = QPixmap().fromImage( QImage( bytes( byte_t_r ), dw, dh, qimage_format ) )

        # Data
        self.channel_data = list()
        if length >= 1: self.channel_data.append( { "render" : pix_0_r, "map" : byte_0_m, "dx" : dx, "dy" : dy, "dw" : dw, "dh" : dh, "text" : chan_0, "cor" : False } )
        if length >= 2: self.channel_data.append( { "render" : pix_1_r, "map" : byte_1_m, "dx" : dx, "dy" : dy, "dw" : dw, "dh" : dh, "text" : chan_1, "cor" : False } )
        if length >= 3: self.channel_data.append( { "render" : pix_2_r, "map" : byte_2_m, "dx" : dx, "dy" : dy, "dw" : dw, "dh" : dh, "text" : chan_2, "cor" : False } )
        if length >= 4: self.channel_data.append( { "render" : pix_3_r, "map" : byte_3_m, "dx" : dx, "dy" : dy, "dw" : dw, "dh" : dh, "text" : chan_3, "cor" : False } )
        self.channel_data.append( { "render" : pix_a_r, "map" : byte_a_m, "dx" : dx, "dy" : dy, "dw" : dw, "dh" : dh, "text" : "Alpha", "cor" : False } )
        if self.tic_display == True: self.channel_data.append( { "render" : pix_t_r, "map":byte_t_m, "dx" : dx, "dy" : dy, "dw" : dw, "dh" : dh, "text":"TIC", "cor" : cor } )

        # List
        self.Channel_Index( previous )
        self.Channel_Icons()
        self.Widget_Insert( True )
    else:
        Message_Warnning( "ERROR", f"Model { cmodel } and/or Depth { d_cd } not supported" )
def Run_Range( self, cmodel, d_cd, depth, k, dx, dy, dw, dh, num_array, num_ss ):
    # Variables
    numbers = list()

    # Length
    length = 3
    if   self.color_space in [ "A", "GRAY" ]: length = 1
    elif self.color_space == "CMYK":          length = 4

    # Points
    r0pa = self.range_0_pa; r1pa = self.range_1_pa; r2pa = self.range_2_pa; r3pa = self.range_3_pa
    r0pb = self.range_0_pb; r1pb = self.range_1_pb; r2pb = self.range_2_pb; r3pb = self.range_3_pb
    r0pc = self.range_0_pc; r1pc = self.range_1_pc; r2pc = self.range_2_pc; r3pc = self.range_3_pc
    r0pd = self.range_0_pd; r1pd = self.range_1_pd; r2pd = self.range_2_pd; r3pd = self.range_3_pd

    # Calculation
    hue_rgb = [ "HSV", "HSL", "HCY", "ARD" ]
    hue_xyz = [ "LCH" ]
    index = 0
    sel_pixels = list()
    draw_pixels = list()

    # Progress Bar
    self.ProgressBar_Maximum( dh )
    self.ProgressBar_Value( 1 )

    # Cycle
    for y in range( 0, dh ):
        # Progress Bar
        y1 = y + 1
        percent = int( ( y / dh ) * 100 )
        if ( percent % 2 ) == 0:
            self.ProgressBar_Value( y1 )
            QApplication.processEvents()
        if numbers == None:break

        # Pixels
        for x in range( 0, dw ):
            # Read Bytes
            numbers = self.analyse.Numbers_on_Pixel( cmodel, d_cd, index, num_array )
            if numbers == None:break
            ssi = num_ss[ index ] / depth

            # Convert
            if cmodel in [ "A", "GRAY" ]:
                conv = self.convert.color_convert( cmodel, self.color_space, [ numbers[0] / depth ] )
                alpha = numbers[1] / depth
            if cmodel in [ "SRGB", "LRGB" ]:
                conv = self.convert.color_convert( cmodel, self.color_space, [ numbers[0] / depth, numbers[1] / depth, numbers[2] / depth ] )
                alpha = numbers[3] / depth
            if cmodel == "CMYK":
                conv = self.convert.color_convert( cmodel, self.color_space, [ numbers[0] / depth, numbers[1] / depth, numbers[2] / depth, numbers[3] / depth ] )
                alpha = numbers[4] / depth

            # Variables
            occlusion = alpha * ssi
            if length == 1:
                # Parse
                n0 = conv[0]
                # Selection 0
                sel_0 = self.analyse.Selector_Linear( n0, r0pa, r0pb, r0pc, r0pd )
                # Factor
                sel_factor = sel_0
            elif length == 3:
                # Parse
                n0 = conv[0]
                n1 = conv[1]
                n2 = conv[2]
                # Selection 0
                if self.color_space in hue_rgb: sel_0 = self.analyse.Selector_Circular( n0, r0pa, r0pb, r0pc, r0pd )
                else:                           sel_0 = self.analyse.Selector_Linear( n0, r0pa, r0pb, r0pc, r0pd )
                # Selection 1
                sel_1 = self.analyse.Selector_Linear( n1, r1pa, r1pb, r1pc, r1pd )
                # Selection 2
                if self.color_space in hue_xyz: sel_2 = self.analyse.Selector_Circular( n2, r2pa, r2pb, r2pc, r2pd )
                else:                           sel_2 = self.analyse.Selector_Linear( n2, r2pa, r2pb, r2pc, r2pd )
                # Factor
                if self.color_space in hue_rgb:     sel_factor = sel_0 * ( ( sel_1 + sel_2 ) / 2 ) # Multiplication
                elif self.color_space in hue_xyz:   sel_factor = ( ( sel_0 + sel_1 ) / 2 ) * sel_2 # Multiplication
                else:                               sel_factor = ( sel_0 + sel_1 + sel_2 ) / 3 # Additive
            elif length == 4:
                # Parse
                n0 = conv[0]
                n1 = conv[1]
                n2 = conv[2]
                n3 = conv[3]
                # Selection
                sel_0 = self.analyse.Selector_Linear( n0, r0pa, r0pb, r0pc, r0pd )
                sel_1 = self.analyse.Selector_Linear( n1, r1pa, r1pb, r1pc, r1pd )
                sel_2 = self.analyse.Selector_Linear( n2, r2pa, r2pb, r2pc, r2pd )
                sel_3 = self.analyse.Selector_Linear( n3, r3pa, r3pb, r3pc, r3pd )
                # Factor
                # sel_factor = sel_0 * sel_1 * sel_2 * sel_3 # Multiplication
                sel_factor = ( sel_0 + sel_1 + sel_2 + sel_3 ) / 4 # Additive
                
            # Lists
            sel_factor *= occlusion
            s = int( sel_factor * k )
            sel_pixels.append( s )
            v = int( sel_factor * 255 )
            o = int( occlusion * 255 )
            draw_pixels.extend( [ v, v, v, o ] )

            # Cycle
            index += 1

    # Progress Bar
    self.ProgressBar_Value( dh )
    QApplication.processEvents()

    # Selection Mask
    if numbers != None and len( sel_pixels ) > 0:
        # Preview
        qimage_format = QImage.Format_RGBA8888
        qpixmap = QPixmap().fromImage( QImage( bytes( draw_pixels ), dw, dh, qimage_format ) )
        self.display_map.Update_Display( qpixmap )
        # Data
        data = { "render" : qpixmap, "map" : sel_pixels, "dx" : dx, "dy" : dy, "dw" : dw, "dh" : dh, "text" : "Color Range", "cor" : False }
        self.range_data = [ data ]
        # List
        self.Display_Map( qpixmap, False )
        self.Widget_Insert( True )
    else:
        Message_Warnning( "ERROR", f"Model { cmodel } and/or Depth { d_cd } not supported" )

"""
To Do:
- Sampler only works in U8 and U16.
- Sampler now can accept images from Imagine Board to apply LUT files (*.cube) too. Can Batch multiple images to the same LUT.
- Color Collection mode: White Colors and Black Colors
"""
