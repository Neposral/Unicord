﻿using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Foundation;
using Windows.Foundation.Collections;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Data;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Navigation;

// The Content Dialog item template is documented at https://go.microsoft.com/fwlink/?LinkId=234238

namespace Unicord.Universal.Dialogs
{
    public sealed partial class YesNoDialog : ContentDialog
    {
        public string Icon { get => iconText.Text; set => iconText.Text = value; }
        public new string Title { get => questionTitle.Text; set => questionTitle.Text = value; }
        public new string Content { get => questionContent.Text; set => questionContent.Text = value; }

        public YesNoDialog()
        {
            InitializeComponent();
        }
    }
}
